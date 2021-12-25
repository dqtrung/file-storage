#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/iostreams/device/back_inserter.hpp>
#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/serialization/string.hpp>

#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>

#include <websocketpp/common/thread.hpp>
#include <websocketpp/common/memory.hpp>

#include <boost/algorithm/string.hpp>

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <fstream>
#include <sstream>

#include <transfering_package.hpp>

typedef websocketpp::client<websocketpp::config::asio_tls_client> client;
typedef websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context> context_ptr;

using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;
using websocketpp::lib::bind;

using std::cout;
using std::endl;

class TlsVerification
{
public:
        bool verify_subject_alternative_name(const char * hostname, X509 * cert) {
            STACK_OF(GENERAL_NAME) * san_names = NULL;

            san_names = (STACK_OF(GENERAL_NAME) *) X509_get_ext_d2i(cert, NID_subject_alt_name, NULL, NULL);
            if (san_names == NULL) {
                return false;
            }

            int san_names_count = sk_GENERAL_NAME_num(san_names);

            bool result = false;

            for (int i = 0; i < san_names_count; i++) {
                const GENERAL_NAME * current_name = sk_GENERAL_NAME_value(san_names, i);

                if (current_name->type != GEN_DNS) {
                    continue;
                }

                char const * dns_name = (char const *) ASN1_STRING_get0_data(current_name->d.dNSName);

                if (ASN1_STRING_length(current_name->d.dNSName) != strlen(dns_name)) {
                    break;
                }
                result = (strcasecmp(hostname, dns_name) == 0);
            }
            sk_GENERAL_NAME_pop_free(san_names, GENERAL_NAME_free);

            return result;
        }

        bool verify_common_name(char const * hostname, X509 * cert) {
            int common_name_loc = X509_NAME_get_index_by_NID(X509_get_subject_name(cert), NID_commonName, -1);
            if (common_name_loc < 0) {
                return false;
            }

            X509_NAME_ENTRY * common_name_entry = X509_NAME_get_entry(X509_get_subject_name(cert), common_name_loc);
            if (common_name_entry == NULL) {
                return false;
            }

            ASN1_STRING * common_name_asn1 = X509_NAME_ENTRY_get_data(common_name_entry);
            if (common_name_asn1 == NULL) {
                return false;
            }

            char const * common_name_str = (char const *) ASN1_STRING_get0_data(common_name_asn1);

            if (ASN1_STRING_length(common_name_asn1) != strlen(common_name_str)) {
                return false;
            }

            return (strcasecmp(hostname, common_name_str) == 0);
        }

        bool verify_certificate(const char * hostname, bool preverified, boost::asio::ssl::verify_context& ctx) {
            int depth = X509_STORE_CTX_get_error_depth(ctx.native_handle());

            if (depth == 0 && preverified) {
                X509* cert = X509_STORE_CTX_get_current_cert(ctx.native_handle());

                if (verify_subject_alternative_name(hostname, cert)) {
                    return true;
                } else if (verify_common_name(hostname, cert)) {
                    return true;
                } else {
                    return false;
                }
            }

            return preverified;
        }
};

class connection_metadata {
    public:
        typedef websocketpp::lib::shared_ptr<connection_metadata> ptr;

        connection_metadata(int id, websocketpp::connection_hdl hdl, std::string uri)
            : m_id(id)
              , m_hdl(hdl)
              , m_status("Connecting")
              , m_uri(uri)
              , m_server("N/A")
    {}

        void on_open(client * c, websocketpp::connection_hdl hdl) {
            m_status = "Open";

            client::connection_ptr con = c->get_con_from_hdl(hdl);
            m_server = con->get_response_header("Server");
        }

        void on_fail(client * c, websocketpp::connection_hdl hdl) {
            m_status = "Failed";

            client::connection_ptr con = c->get_con_from_hdl(hdl);
            m_server = con->get_response_header("Server");
            m_error_reason = con->get_ec().message();
        }

        void on_close(client * c, websocketpp::connection_hdl hdl) {
            m_status = "Closed";
            client::connection_ptr con = c->get_con_from_hdl(hdl);
            std::stringstream s;
            s << "close code: " << con->get_remote_close_code() << " ("
                << websocketpp::close::status::get_string(con->get_remote_close_code())
                << "), close reason: " << con->get_remote_close_reason();
            m_error_reason = s.str();
        }

        void on_message(websocketpp::connection_hdl, client::message_ptr msg) {
            if (msg->get_opcode() == websocketpp::frame::opcode::text) {
                m_messages.push_back("<< " + msg->get_payload());
                std::cout << "Received text from server: " << msg->get_payload();
            } else {
                m_messages.push_back("<< " + websocketpp::utility::to_hex(msg->get_payload()));
                std::cout << "Received hex from server: " << websocketpp::utility::to_hex(msg->get_payload());
            }
        }

        context_ptr on_tls_init(websocketpp::connection_hdl) {
            context_ptr ctx = websocketpp::lib::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::sslv23);

            try {
                ctx->set_options(boost::asio::ssl::context::default_workarounds |
                        boost::asio::ssl::context::no_sslv2 |
                        boost::asio::ssl::context::no_sslv3 |
                        boost::asio::ssl::context::single_dh_use);

                ctx->set_verify_mode(boost::asio::ssl::verify_none);
                ctx->load_verify_file("server.pem");
            } catch (std::exception& e) {
                std::cout << e.what() << std::endl;
            }
            return ctx;
        }

        websocketpp::connection_hdl get_hdl() const {
            return m_hdl;
        }

        int get_id() const {
            return m_id;
        }

        std::string get_status() const {
            return m_status;
        }

        void record_sent_message(std::string message) {
            m_messages.push_back(">> " + message);
        }

        friend std::ostream & operator<< (std::ostream & out, connection_metadata const & data);
    private:
        int m_id;
        websocketpp::connection_hdl m_hdl;
        std::string m_status;
        std::string m_uri;
        std::string m_server;
        std::string m_error_reason;
        std::vector<std::string> m_messages;
};

std::ostream & operator<< (std::ostream & out, connection_metadata const & data) {
    out << "> URI: " << data.m_uri << "\n"
        << "> Status: " << data.m_status << "\n"
        << "> Remote Server: " << (data.m_server.empty() ? "None Specified" : data.m_server) << "\n"
        << "> Error/close reason: " << (data.m_error_reason.empty() ? "N/A" : data.m_error_reason) << "\n";
    out << "> Messages Processed: (" << data.m_messages.size() << ") \n";

    std::vector<std::string>::const_iterator it;
    for (it = data.m_messages.begin(); it != data.m_messages.end(); ++it) {
        out << *it << "\n";
    }

    return out;
}

class websocket_endpoint {
    public:
        websocket_endpoint () : m_next_id(0) {
            m_endpoint.clear_access_channels(websocketpp::log::alevel::all);
            m_endpoint.clear_error_channels(websocketpp::log::elevel::all);
            m_endpoint.set_error_channels(websocketpp::log::elevel::all);

            m_endpoint.init_asio();
            m_endpoint.start_perpetual();

            m_thread = websocketpp::lib::make_shared<websocketpp::lib::thread>(&client::run, &m_endpoint);
        }

        ~websocket_endpoint() {
            m_endpoint.stop_perpetual();

            for (con_list::const_iterator it = m_connection_list.begin(); it != m_connection_list.end(); ++it) {
                if (it->second->get_status() != "Open") {
                    // Only close open connections
                    continue;
                }

                std::cout << "> Closing connection " << it->second->get_id() << std::endl;

                websocketpp::lib::error_code ec;
                m_endpoint.close(it->second->get_hdl(), websocketpp::close::status::going_away, "", ec);
                if (ec) {
                    std::cout << "> Error closing connection " << it->second->get_id() << ": "
                        << ec.message() << std::endl;
                }
            }


            m_thread->join();
        }

        context_ptr on_tls_init(websocketpp::connection_hdl) {
            context_ptr ctx = websocketpp::lib::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::sslv23);

            try {
                ctx->set_options(boost::asio::ssl::context::default_workarounds |
                        boost::asio::ssl::context::no_sslv2 |
                        boost::asio::ssl::context::no_sslv3 |
                        boost::asio::ssl::context::single_dh_use);

                ctx->set_verify_mode(boost::asio::ssl::verify_none);
                ctx->load_verify_file("server.pem");
            } catch (std::exception& e) {
                std::cout << e.what() << std::endl;
            }
            return ctx;
        }

        void on_fail(websocketpp::connection_hdl hdl) {
            client::connection_ptr con = m_endpoint.get_con_from_hdl(hdl);
            std::cout << "Fail " << con->get_ec().message() << std::endl;
        }


        int connect(std::string const & uri) {
            websocketpp::lib::error_code ec;

            m_endpoint.set_tls_init_handler(bind(&websocket_endpoint::on_tls_init, this, ::_1));
            m_endpoint.set_fail_handler(bind(&websocket_endpoint::on_fail, this, ::_1));
            client::connection_ptr con = m_endpoint.get_connection(uri, ec);

            if (ec) {
                std::cout << "> Connect initialization error: " << ec.message() << std::endl;
                return -1;
            }

            int new_id = m_next_id++;
            connection_metadata::ptr metadata_ptr = websocketpp::lib::make_shared<connection_metadata>(new_id, con->get_handle(), uri);
            m_connection_list[new_id] = metadata_ptr;

            con->set_open_handler(websocketpp::lib::bind(
                        &connection_metadata::on_open,
                        metadata_ptr,
                        &m_endpoint,
                        websocketpp::lib::placeholders::_1
                        ));
            con->set_fail_handler(websocketpp::lib::bind(
                        &connection_metadata::on_fail,
                        metadata_ptr,
                        &m_endpoint,
                        websocketpp::lib::placeholders::_1
                        ));
            con->set_close_handler(websocketpp::lib::bind(
                        &connection_metadata::on_close,
                        metadata_ptr,
                        &m_endpoint,
                        websocketpp::lib::placeholders::_1
                        ));
            con->set_message_handler(websocketpp::lib::bind(
                        &connection_metadata::on_message,
                        metadata_ptr,
                        websocketpp::lib::placeholders::_1,
                        websocketpp::lib::placeholders::_2
                        ));

            m_endpoint.connect(con);

            return new_id;
        }

        void close(int id, websocketpp::close::status::value code, std::string reason) {
            websocketpp::lib::error_code ec;

            con_list::iterator metadata_it = m_connection_list.find(id);
            if (metadata_it == m_connection_list.end()) {
                std::cout << "> No connection found with id " << id << std::endl;
                return;
            }

            m_endpoint.close(metadata_it->second->get_hdl(), code, reason, ec);
            if (ec) {
                std::cout << "> Error initiating close: " << ec.message() << std::endl;
            }
        }

        /*
        void send(int id, std::string message) {
            websocketpp::lib::error_code ec;

            con_list::iterator metadata_it = m_connection_list.find(id);
            if (metadata_it == m_connection_list.end()) {
                std::cout << "> No connection found with id " << id << std::endl;
                return;
            }

            m_endpoint.send(metadata_it->second->get_hdl(), message, websocketpp::frame::opcode::text, ec);
            if (ec) {
                std::cout << "> Error sending message: " << ec.message() << std::endl;
                return;
            }

            metadata_it->second->record_sent_message(message);
        }
        */

        void send(int id, std::string file_path) {
            websocketpp::lib::error_code ec;

            con_list::iterator metadata_it = m_connection_list.find(id);
            if (metadata_it == m_connection_list.end()) {
                std::cout << "> No connection found with id " << id << std::endl;
                return;
            }

            boost::trim(file_path);
            std::ifstream fin{file_path, std::ios::in | std::ios::binary};
            std::cout << "Reading:" << file_path << std::endl;

            std::istreambuf_iterator<char> it(fin);
            std::istreambuf_iterator<char> end;

            std::vector<char> bytes;
            while(it != end) {
                bytes.push_back(*it);
                ++it;
            }

//            std::vector<char> bytes(
//                    (std::istreambuf_iterator<char>(fin)),
//                    (std::istreambuf_iterator<char>()));
//            size_t size = bytes.size();
//            char* buf = bytes.data();
            fin.close();

            TransferingPackage data{"req_id=12345465", std::move(bytes)};
            std::vector<char> v;
            boost::iostreams::back_insert_device<std::vector<char>> sink{v};
            boost::iostreams::stream<boost::iostreams::back_insert_device<std::vector<char>>> os{sink};
            boost::archive::binary_oarchive out_archive(os);
            out_archive << data;

            size_t size = v.size();
            char* buf = v.data();

            client::connection_ptr con = m_endpoint.get_con_from_hdl(metadata_it->second->get_hdl());
            client::message_ptr msg = con->get_message(websocketpp::frame::opcode::binary, size);
            cout <<  "Set payload \n";
//            cout << buf << endl;
            msg->set_payload(buf, size);
//            cout <<  "Set opcode \n";
//            msg->set_opcode(websocketpp::frame::opcode::binary);
            cout <<  "Set header \n";
            msg->set_header("req_id=12345");

            ec = con->send(msg);

            std::cout << "Sending:" << file_path << " with size " << size << std::endl;
            //cout << buf << endl;
//            m_endpoint.send(metadata_it->second->get_hdl(), buf, size, websocketpp::frame::opcode::binary, ec);

            if (ec) {
                std::cout << "> Error sending message: " << ec.message() << std::endl;
                return;
            }
        }

        connection_metadata::ptr get_metadata(int id) const {
            con_list::const_iterator metadata_it = m_connection_list.find(id);
            if (metadata_it == m_connection_list.end()) {
                return connection_metadata::ptr();
            } else {
                return metadata_it->second;
            }
        }

    private:
        typedef std::map<int,connection_metadata::ptr> con_list;

        client m_endpoint;
        websocketpp::lib::shared_ptr<websocketpp::lib::thread> m_thread;

        con_list m_connection_list;
        int m_next_id;
};

int main() {
    bool done = false;
    std::string input;
    websocket_endpoint endpoint;

    while (!done) {
        std::cout << "Enter Command: ";
        std::getline(std::cin, input);

        if (input == "quit") {
            done = true;
        } else if (input == "help") {
            std::cout
                << "\nCommand List:\n"
                << "connect <ws uri>\n"
                << "send <connection id> <message>\n"
                << "close <connection id> [<close code:default=1000>] [<close reason>]\n"
                << "show <connection id>\n"
                << "help: Display this help text\n"
                << "quit: Exit the program\n"
                << std::endl;
        } else if (input.substr(0,7) == "connect") {
            int id = endpoint.connect(input.substr(8));
            if (id != -1) {
                std::cout << "> Created connection with id " << id << std::endl;
            }
        } else if (input.substr(0,4) == "send") {
            std::stringstream ss(input);

            std::string cmd;
            int id;
            std::string message;

            ss >> cmd >> id;
            std::getline(ss,message);

            endpoint.send(id, message);
        } else if (input.substr(0,5) == "close") {
            std::stringstream ss(input);

            std::string cmd;
            int id;
            int close_code = websocketpp::close::status::normal;
            std::string reason;

            ss >> cmd >> id >> close_code;
            std::getline(ss,reason);

            endpoint.close(id, close_code, reason);
        } else if (input.substr(0,4) == "show") {
            int id = atoi(input.substr(5).c_str());

            connection_metadata::ptr metadata = endpoint.get_metadata(id);
            if (metadata) {
                std::cout << *metadata << std::endl;
            } else {
                std::cout << "> Unknown connection id " << id << std::endl;
            }
        } else {
            std::cout << "> Unrecognized Command" << std::endl;
        }
    }

    return 0;
}
