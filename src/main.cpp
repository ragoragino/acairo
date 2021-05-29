#include <iostream>
#include <chrono>
#include <vector>
#include <string>
#include <csignal>

#include <cairo.h>

volatile sig_atomic_t sig_flag = 0;

void handle_socket(cairo::TCPStream&& stream) {
    try {
        const std::string send_message = "Is there anybody out there?";
        std::vector<char> vector_message(send_message.begin(), send_message.end());
        stream.write(std::move(vector_message));

        std::cout << "Writing to socket was successful." << "\n"; 

        std::vector<char> received_message = stream.read(4);

        std::cout << "Reading from socket was succesful." << "\n"; 
    } catch (std::exception& e){
        std::cout << "handle_socket failed: " << e.what() << "\n"; 
    }
}

int main(){
    using namespace std::chrono_literals;

    cairo::ExecutorConfiguration executor_config{
        number_of_worker_threads: 10,
    };

    cairo::TCPStreamConfiguration tcpstream_configuration{
        read_timeout: 5s,
        write_timeout: 5s,
    };

    cairo::TCPListenerConfiguration tcplistener_config{
        stream_config: tcpstream_configuration,
        max_number_of_fds: 1024,
        max_number_of_queued_conns: 1024,
    };

    auto executor = cairo::Executor(executor_config);
    auto listener = cairo::TCPListener(tcplistener_config);

    // https://www.informit.com/articles/article.aspx?p=2204014
    if (std::signal(SIGINT, [](int) -> void {
        sig_flag = 1;
    }) == SIG_ERR) {
        std::cout << "Unable to register signal handler.\n";
        exit(1);
    }

    while(sig_flag == 0) {
        cairo::TCPStream stream = listener.accept();

        auto handler = [&stream](){ 
            handle_socket(std::move(stream));
        };

        executor.spawn(cairo::Awaitable(handler));
    }

    std::cout << "Shutting down cairo.\n";

    listener.shutdown(5s);

    executor.stop();

    return 0;
}