import reactor;
import net.tcp_socket_driver;

int main()
{
    etsl::C_Reactor reactor;
    if (const auto err = reactor.initialize(); !err) {
        return err.error();
    }

    reactor.run();
    return 0;
}
