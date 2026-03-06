#include "async_io.hpp"

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

using as::async_read;
using as::async_write;
using as::io_context;
using as::task;

task<void> writer(io_context& ctx, int fd, std::string_view message) {
    co_await ctx.sleep_for(std::chrono::milliseconds(100));
    auto written = co_await async_write(ctx, fd, message.data(), message.size());
    std::cout << "writer sent " << written << " bytes\n";
}

task<void> reader(io_context& ctx, int fd) {
    std::array<char, 256> buf{};
    auto n = co_await async_read(ctx, fd, buf.data(), buf.size());
    if (n == 0) {
        std::cout << "reader: EOF\n";
        co_return;
    }
    std::string received(buf.data(), static_cast<size_t>(n));
    std::cout << "reader got: " << received << "\n";
}

task<void> yielder(io_context& ctx, std::string& trace, char tag, int rounds) {
    for (int i = 0; i < rounds; ++i) {
        trace.push_back(tag);
        co_await ctx.yield_now();
    }
}

int main() {
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        throw std::runtime_error(std::strerror(errno));
    }

    try {
        as::set_non_blocking(fds[0]);
        as::set_non_blocking(fds[1]);

        io_context ctx;
        ctx.co_spawn(writer(ctx, fds[0], "hello from coroutine io"));
        ctx.co_spawn(reader(ctx, fds[1]));
        ctx.run();

        io_context yield_ctx;
        std::string trace;
        yield_ctx.co_spawn(yielder(yield_ctx, trace, 'A', 3));
        yield_ctx.co_spawn(yielder(yield_ctx, trace, 'B', 3));
        yield_ctx.run();

        std::cout << "yield trace: " << trace << "\n";
        if (trace != "ABABAB") {
            throw std::runtime_error("yield scheduling mismatch: " + trace);
        }

        ::close(fds[0]);
        ::close(fds[1]);
        return 0;
    } catch (...) {
        ::close(fds[0]);
        ::close(fds[1]);
        throw;
    }
}
