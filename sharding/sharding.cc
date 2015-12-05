//
// Created by wastl on 19.11.15.
//
#include <thread>
#include <glog/logging.h>

#include <grpc++/channel.h>
#include <grpc++/client_context.h>
#include <grpc++/create_channel.h>
#include <grpc++/security/credentials.h>
#include <grpc++/support/sync_stream.h>

#include "sharding/sharding.h"
#include "model/rdf_model.h"
#include "model/rdf_operators.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReader;
using grpc::ClientReaderWriter;
using grpc::ClientWriter;
using grpc::Status;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerWriter;
using marmotta::rdf::proto::Namespace;
using marmotta::rdf::proto::Statement;
using marmotta::service::proto::ContextRequest;
using marmotta::service::proto::SailService;
using marmotta::service::proto::UpdateRequest;
using marmotta::service::proto::UpdateResponse;
using google::protobuf::Int64Value;

namespace marmotta {
namespace sharding {

// A templated fanout function, forwarding the same request to all backends and collecting
// Int64Value responses by summing them up.
template<typename Request,
        Status (SailService::Stub::*ClientMethod)(ClientContext*, const Request&, Int64Value*)>
Status Fanout(const Request& request, std::vector<std::string> &backends, Int64Value *result) {
    std::vector<std::thread> threads;
    std::vector<Status> statuses(backends.size());

    int64_t r = 0;
    for (int i=0; i<backends.size(); i++) {
        threads.push_back(std::thread([i, &backends, &statuses, &request, &r]() {
            ClientContext localctx;
            Int64Value response;
            auto stub = svc::SailService::NewStub(
                    grpc::CreateChannel(backends[i], grpc::InsecureChannelCredentials()));
            statuses[i] = ((*stub).*ClientMethod)(&localctx, request, &response);
            r += response.value();
        }));
    }

    // need to wait until all are completed now.
    for (auto& t : threads) {
        t.join();
    }

    result->set_value(r);

    for (auto s : statuses) {
        if (!s.ok())
            return s;
    }

    return Status::OK;
};

ShardingService::ShardingService(std::vector<std::string> backends) : backends(backends) { }

grpc::Status ShardingService::AddNamespaces(
        ServerContext *context, ServerReader<Namespace> *reader, Int64Value *result) {

    std::vector<ClientContext> contexts(backends.size());
    std::vector<Int64Value> stats(backends.size());

    StubList stubs;
    WriterList <Namespace> writers;

    for (int i=0; i<backends.size(); i++) {
        stubs.push_back(makeStub(i));
        writers.push_back(stubs.back()->AddNamespaces(&contexts[i], &stats[i]));
    }

    // Iterate over all namespaces and schedule a write task.
    Namespace ns;
    while (reader->Read(&ns)) {
        for (auto& w : writers) {
            w->Write(ns);
        }
    }

    for (auto& w : writers) {
        w->WritesDone();
        w->Finish();
    }

    result->set_value(stats[0].value());

    return Status::OK;
}

grpc::Status ShardingService::AddStatements(grpc::ServerContext *context,
                                            grpc::ServerReader<Statement> *reader,
                                            Int64Value *result) {
    std::vector<ClientContext> contexts(backends.size());
    std::vector<Int64Value> responses(backends.size());

    StubList stubs;
    WriterList<Statement> writers;
    for (int i=0; i<backends.size(); i++) {
        stubs.push_back(makeStub(i));
        writers.push_back(Writer<Statement>(
                stubs.back()->AddStatements(&contexts[i], &responses[i])));
    }

    std::hash<Statement> stmt_hash;

    Statement stmt;
    std::string buf;
    while (reader->Read(&stmt)) {
            size_t bucket = stmt_hash(stmt) % backends.size();
            writers[bucket]->Write(stmt);
    }
    for (auto& w : writers) {
        w->WritesDone();
        w->Finish();
    }

    for (auto& r : responses) {
        result->set_value(result->value() + r.value());
    }

    return Status::OK;
}

grpc::Status ShardingService::GetStatements(
        ServerContext *context, const Statement *pattern, ServerWriter<Statement> *result) {
    std::vector<std::thread> threads;
    std::mutex mutex;

    for (int i=0; i<backends.size(); i++) {
        threads.push_back(std::thread([i, this, &mutex, result, pattern]() {
            DLOG(INFO) << "Getting statements from shard " << i << " started";
            ClientContext localctx;
            auto stub = makeStub(i);
            auto reader = stub->GetStatements(&localctx, *pattern);

            int64_t count = 0;
            Statement stmt;
            while (reader->Read(&stmt)) {
                std::lock_guard<std::mutex> guard(mutex);
                result->Write(stmt);
                count++;
            }
            DLOG(INFO) << "Getting statements from shard " << i << " finished (" << count << " results)";
        }));
    }

    for (auto& t : threads) {
        t.join();
    }


    return Status::OK;
}

Status ShardingService::RemoveStatements(
        ServerContext *context, const Statement *pattern, Int64Value *result) {
    return Fanout<Statement, &SailService::Stub::RemoveStatements>(*pattern, backends, result);
}


Status ShardingService::Update(
        ServerContext *context, ServerReader<UpdateRequest> *reader, UpdateResponse *result) {
    std::vector<ClientContext> contexts(backends.size());
    std::vector<UpdateResponse> responses(backends.size());

    StubList stubs;
    WriterList <UpdateRequest> writers;

    for (int i=0; i<backends.size(); i++) {
        stubs.push_back(makeStub(i));
        writers.push_back(stubs.back()->Update(&contexts[i], &responses[i]));
    }

    std::hash<Statement> stmt_hash;
    std::hash<Namespace> ns_hash;

    UpdateRequest req;
    std::string buf;
    while (reader->Read(&req)) {
        if (req.has_stmt_added()) {
            size_t bucket = stmt_hash(req.stmt_added()) % backends.size();
            writers[bucket]->Write(req);
        }
        if (req.has_stmt_removed()) {
            size_t bucket = stmt_hash(req.stmt_removed()) % backends.size();
            writers[bucket]->Write(req);
        }
        if (req.has_ns_added() || req.has_ns_removed()) {
            for (auto& w : writers) {
                w->Write(req);
            }
        }
    }
    for (auto& w : writers) {
        w->WritesDone();
        w->Finish();
    }

    for (auto& r : responses) {
        result->set_added_namespaces(result->added_namespaces() + r.added_namespaces());
        result->set_removed_namespaces(result->removed_namespaces() + r.removed_namespaces());
        result->set_added_statements(result->added_statements() + r.added_statements());
        result->set_removed_statements(result->removed_statements() + r.removed_statements());
    }


    return Status::OK;
}

Status ShardingService::Clear(
        ServerContext *context, const ContextRequest *contexts, Int64Value *result) {
    return Fanout<ContextRequest, &SailService::Stub::Clear>(*contexts, backends, result);
}

Status ShardingService::Size(
        ServerContext *context, const ContextRequest *contexts, Int64Value *result) {
    return Fanout<ContextRequest, &SailService::Stub::Size>(*contexts, backends, result);
}

std::unique_ptr<SailService::Stub> ShardingService::makeStub(int i) {
    return SailService::NewStub(
            grpc::CreateChannel(backends[i], grpc::InsecureChannelCredentials()));
}
}
}