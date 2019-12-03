// Copyright 2010-2015 RethinkDB, all rights reserved.
#ifndef CLUSTERING_IMMEDIATE_CONSISTENCY_REMOTE_REPLICATOR_SERVER_HPP_
#define CLUSTERING_IMMEDIATE_CONSISTENCY_REMOTE_REPLICATOR_SERVER_HPP_

#include "clustering/immediate_consistency/primary_dispatcher.hpp"
#include "clustering/immediate_consistency/remote_replicator_metadata.hpp"
#include "clustering/generic/registrar.hpp"

/* `remote_replicator_server_t` takes reads and writes from the `primary_dispatcher_t`
and sends them over the network to `remote_replicator_client_t`s on other machines.

There is one `remote_replicator_server_t` per shard. It lives on the primary replica
server with the `primary_dispatcher_t`. */

class remote_replicator_server_t {
public:
    remote_replicator_server_t(
        mailbox_manager_t *mailbox_manager,
        primary_dispatcher_t *primary);
    ~remote_replicator_server_t();

    remote_replicator_server_bcard_t get_bcard() {
        return remote_replicator_server_bcard_t {
            primary->get_branch_id(),
            primary->get_branch_birth_certificate().get_region(),
            registrar.get_business_card() };
    }

private:
    /* Whenever a `remote_replicator_client_t` connects, the `registrar` will construct a
    `proxy_replica_t` to represent it. */
    class proxy_replica_t;

    mailbox_manager_t *mailbox_manager;
    primary_dispatcher_t *primary;

    registrar_t<
        remote_replicator_client_bcard_t,
        remote_replicator_server_t *,
        proxy_replica_t> registrar;
};

#endif /* CLUSTERING_IMMEDIATE_CONSISTENCY_REMOTE_REPLICATOR_SERVER_HPP_ */

