// Unit tests for the frame ring slot logic (protocol.h / ring.h). Simulates the
// producer consumer dance without a GPU: the producer must never write the buffer
// that is published as latest or reserved by the reader.
#include <cstdio>

#include "ring.h"

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do                                                                       \
    {                                                                        \
        if (!(cond))                                                         \
        {                                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

int main()
{
    // Exhaustive: for every reachable (latest, reader) pair a valid, distinct slot exists.
    for (long latest = -1; latest < SD_BUFFERS; latest++)
        for (long reader = -1; reader < SD_BUFFERS; reader++)
        {
            int w = SdPickWriteSlot(latest, reader);
            CHECK(w >= 0 && w < SD_BUFFERS);
            CHECK(w != latest && w != reader);
        }

    // Corrupted shared state (another process can write the section) is rejected,
    // never turned into an out-of-bounds texture index.
    CHECK(SdPickWriteSlot(-2, 0) == -1);
    CHECK(SdPickWriteSlot(0, SD_BUFFERS) == -1);
    CHECK(SdPickWriteSlot(12345, -999) == -1);

    // A 100k-frame simulation with a reader that holds each reservation for a few
    // publishes: the producer never stomps a reserved or published buffer, and the
    // reader always observes monotonically increasing sequence numbers.
    {
        long latest = -1, reader = -1;
        long long seqOf[SD_BUFFERS] = {};
        long long published = 0, lastRead = 0;
        int hold = 0;
        unsigned rng = 12345;
        auto rnd = [&] { rng = rng * 1664525u + 1013904223u; return rng >> 16; };
        for (int i = 0; i < 100000; i++)
        {
            // Producer publishes a frame.
            int w = SdPickWriteSlot(latest, reader);
            CHECK(w >= 0);
            if (w < 0) break;
            CHECK(seqOf[w] == 0 || w != reader); // w is never the reserved slot
            seqOf[w] = ++published;
            latest = w;

            // Reader finishes its copy after a random hold of 0..3 publishes.
            if (reader >= 0 && --hold <= 0) reader = -1;
            // Reader acquires the newest frame when free.
            if (reader < 0 && latest >= 0 && seqOf[latest] > lastRead)
            {
                reader = latest;
                hold = (int)(rnd() % 4);
                CHECK(seqOf[reader] > lastRead);
                lastRead = seqOf[reader];
            }
        }
        CHECK(lastRead > 90000); // the reader keeps up in aggregate
    }

    std::printf(g_failures ? "%d FAILED\n" : "all ring tests passed\n", g_failures);
    return g_failures ? 1 : 0;
}
