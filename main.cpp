import reactor;

int main()
{
    etsl::C_Reactor reactor;
    if (const auto res = reactor.initialize(); !res) {
        return res.error();
    }

    reactor.run();
    return 0;
}
