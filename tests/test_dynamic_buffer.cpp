// utils::DynamicBuffer: growth policy, tail reservation, commit clamping,
// shrink, clear and move semantics.
#include <cstring>

#include "test_common.h"
#include "uvent/utils/buffer/DynamicBuffer.h"

using usub::uvent::utils::DynamicBuffer;

namespace
{
    void empty_buffer_has_no_storage()
    {
        DynamicBuffer b;
        CHECK_EQ(b.size(), 0u);
        CHECK_EQ(b.capacity(), 0u);
        CHECK(b.data() == nullptr);
        b.clear();
        b.shrink(10);
        CHECK_EQ(b.size(), 0u);
    }

    void reserve_grows_in_powers_of_two_from_4096()
    {
        DynamicBuffer b;
        b.reserve(1);
        CHECK_EQ(b.capacity(), 4096u);
        CHECK_EQ(b.size(), 0u);
        b.reserve(4096);
        CHECK_EQ(b.capacity(), 4096u); // no realloc when it already fits
        b.reserve(4097);
        CHECK_EQ(b.capacity(), 8192u);
        b.reserve(100000);
        CHECK_EQ(b.capacity(), 131072u);
        CHECK((reinterpret_cast<uintptr_t>(b.data()) % 64) == 0); // cache-line aligned storage
    }

    void append_preserves_contents_across_growth()
    {
        DynamicBuffer b;
        std::string expected;
        for (int i = 0; i < 50; ++i)
        {
            std::string chunk(1000, static_cast<char>('A' + (i % 26)));
            b.append(reinterpret_cast<const uint8_t*>(chunk.data()), chunk.size());
            expected += chunk;
        }
        CHECK_EQ(b.size(), expected.size());
        CHECK(b.capacity() >= b.size());
        CHECK(std::memcmp(b.data(), expected.data(), expected.size()) == 0);
    }

    void reserve_tail_and_commit()
    {
        DynamicBuffer b;
        uint8_t* tail = b.reserve_tail(10);
        CHECK(tail != nullptr);
        CHECK_EQ(b.size(), 0u); // reserving does not publish bytes
        std::memset(tail, 'x', 10);
        b.commit(10);
        CHECK_EQ(b.size(), 10u);
        CHECK(b.data()[9] == 'x');

        // reserve_tail after data returns the position right after size().
        uint8_t* tail2 = b.reserve_tail(5);
        CHECK(tail2 == b.data() + 10);

        // commit clamps at capacity instead of overrunning.
        b.commit(1 << 20);
        CHECK_EQ(b.size(), b.capacity());
    }

    void append_raw_publishes_immediately()
    {
        DynamicBuffer b;
        uint8_t* p = b.append_raw(3);
        CHECK_EQ(b.size(), 3u);
        p[0] = 1;
        p[1] = 2;
        p[2] = 3;
        CHECK(b.data()[0] == 1 && b.data()[1] == 2 && b.data()[2] == 3);
        const uint8_t more[] = {4, 5};
        b.append(more, 2);
        CHECK_EQ(b.size(), 5u);
        CHECK(b.data()[4] == 5);
    }

    void shrink_and_clear_keep_capacity()
    {
        DynamicBuffer b;
        const std::string s(3000, 'q');
        b.append(reinterpret_cast<const uint8_t*>(s.data()), s.size());
        const size_t cap = b.capacity();
        b.shrink(5000); // larger than size: no-op
        CHECK_EQ(b.size(), 3000u);
        b.shrink(100);
        CHECK_EQ(b.size(), 100u);
        CHECK_EQ(b.capacity(), cap);
        b.clear();
        CHECK_EQ(b.size(), 0u);
        CHECK_EQ(b.capacity(), cap);
        CHECK(b.data() != nullptr);
    }

    void move_transfers_ownership()
    {
        DynamicBuffer a;
        const uint8_t bytes[] = {9, 8, 7};
        a.append(bytes, 3);
        const uint8_t* storage = a.data();

        DynamicBuffer b(std::move(a));
        CHECK(b.data() == storage);
        CHECK_EQ(b.size(), 3u);
        CHECK(a.data() == nullptr);
        CHECK_EQ(a.size(), 0u);
        CHECK_EQ(a.capacity(), 0u);

        DynamicBuffer c;
        c.append(bytes, 1);
        c = std::move(b); // frees c's old storage, takes b's
        CHECK(c.data() == storage);
        CHECK_EQ(c.size(), 3u);
        CHECK(b.data() == nullptr);

        c = std::move(c); // self-move is a no-op
        CHECK(c.data() == storage);
        CHECK_EQ(c.size(), 3u);
    }
} // namespace

int main()
{
    return run_tests({
        {"empty_buffer_has_no_storage", empty_buffer_has_no_storage},
        {"reserve_grows_in_powers_of_two_from_4096", reserve_grows_in_powers_of_two_from_4096},
        {"append_preserves_contents_across_growth", append_preserves_contents_across_growth},
        {"reserve_tail_and_commit", reserve_tail_and_commit},
        {"append_raw_publishes_immediately", append_raw_publishes_immediately},
        {"shrink_and_clear_keep_capacity", shrink_and_clear_keep_capacity},
        {"move_transfers_ownership", move_transfers_ownership},
    });
}
