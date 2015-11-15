//
// Created by wastl on 15.11.15.
//
#include <fstream>

#include <grpc/grpc.h>
#include <grpc++/channel.h>
#include <grpc++/client_context.h>
#include <grpc++/create_channel.h>
#include <grpc++/security/credentials.h>
#include <grpc++/support/sync_stream.h>

#include <google/protobuf/wrappers.pb.h>

#include "model/rdf_model.h"
#include "parser/rdf_parser.h"
#include "serializer/rdf_serializer.h"
#include "service/sail.pb.h"
#include "service/sail.grpc.pb.h"


using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReader;
using grpc::ClientReaderWriter;
using grpc::ClientWriter;
using grpc::Status;

using namespace marmotta;
namespace svc = marmotta::service::proto;

// A STL iterator wrapper around a client reader.
template <class T, class Proto>
class ClientReaderIterator {
 public:
    ClientReaderIterator() : finished(true) { }

    ClientReaderIterator(ClientReader<Proto>* r) : reader(r), finished(false) {
        // Immediately move to first element.
        operator++();
    }

    ClientReaderIterator& operator++() {
        if (!finished) {
            finished = !reader->Read(&buffer);
            if (finished) {
                reader->Finish();
            }
        }
        return *this;
    }

    T operator*() {
        return T(buffer);
    }

    bool operator==(const ClientReaderIterator<T, Proto>& other) {
        return finished == other.finished;
    }

    bool operator!=(const ClientReaderIterator<T, Proto>& other) {
        return finished != other.finished;
    }

    static ClientReaderIterator<T, Proto> end() {
       return ClientReaderIterator<T, Proto>();
    }

 private:
    ClientReader<Proto>* reader;
    Proto buffer;
    bool finished;
};

typedef ClientReaderIterator<rdf::Statement, rdf::proto::Statement> StatementReader;

class MarmottaClient {
 public:
    MarmottaClient(std::shared_ptr<Channel> channel)
            : stub_(svc::SailService::NewStub(channel)) {}

    void importDataset(std::istream& in, parser::Format format) {
        ClientContext nscontext, stmtcontext;

        google::protobuf::Int64Value nsstats;
        google::protobuf::Int64Value stmtstats;

        std::unique_ptr<ClientWriter<rdf::proto::Namespace> > nswriter(
                stub_->AddNamespaces(&nscontext, &nsstats));
        std::unique_ptr<ClientWriter<rdf::proto::Statement> > stmtwriter(
                stub_->AddStatements(&stmtcontext, &stmtstats));

        parser::Parser p("http://www.example.com", parser::Format::RDFXML);
        p.setStatementHandler([&stmtwriter](const rdf::Statement& stmt) {
            stmtwriter->Write(stmt.getMessage());
        });
        p.setNamespaceHandler([&nswriter](const rdf::Namespace& ns) {
            nswriter->Write(ns.getMessage());
        });
        p.parse(in);

        stmtwriter->WritesDone();
        nswriter->WritesDone();

        Status nsst = nswriter->Finish();
        Status stmtst = stmtwriter->Finish();

        if (nsst.ok() && stmtst.ok()) {
            std::cout << "Added " << nsstats.value() << " namespaces and "
                                  << stmtstats.value() << " statements" << std::endl;
        } else {
            std::cout << "Failed writing data to server: " << stmtst.error_message() << std::endl;
        }
    }


    void queryDataset(const rdf::Statement& pattern, std::ostream& out, serializer::Format format) {
        ClientContext context;

        std::unique_ptr<ClientReader<rdf::proto::Statement> > reader(
            stub_->GetStatements(&context, pattern.getMessage()));

        serializer::Serializer serializer("http://www.example.com", format);
        serializer.serialize(StatementReader(reader.get()), StatementReader::end(), out);
    }

 private:
    std::unique_ptr<svc::SailService::Stub> stub_;
};


int main(int argc, const char** argv) {
    MarmottaClient client(
            grpc::CreateChannel("localhost:10000", grpc::InsecureCredentials()));

    std::ifstream in(argv[1]);
    std::cout << "Importing " << argv[1] << " ... " << std::endl;
    client.importDataset(in, parser::Format::RDFXML);
    std::cout << "Finished!" << std::endl;

}