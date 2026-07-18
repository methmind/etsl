import event_loop;

int main()
{
    etsl::C_EventLoop loop;
    if (const auto res = loop.initialize(); !res) {
        return res.error();
    }

    loop.run();
    return 0;
}
