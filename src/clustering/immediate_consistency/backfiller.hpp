// Copyright 2010-2015 RethinkDB, all rights reserved.
#ifndef CLUSTERING_IMMEDIATE_CONSISTENCY_BACKFILLER_HPP_
#define CLUSTERING_IMMEDIATE_CONSISTENCY_BACKFILLER_HPP_

#include <map>
#include <utility>

#include "clustering/generic/registrar.hpp"
#include "clustering/immediate_consistency/backfill_metadata.hpp"

class backfill_progress_tracker_t;
class branch_history_manager_t;
class mailbox_manager_t;
class store_view_t;

/* `backfiller_t` is responsible for copying the given store's state to other servers via
`backfillee_t`.

It assumes that if the state of the underlying store changes, the only change will be to
apply writes along the current branch. In particular, it might break if the underlying
store receives a backfill, changes branches, or erases data while the `backfiller_t`
exists. (If the underlying store is a `store_subview_t`, it's OK if other changes happen
to the underlying store's underlying store outside of the region covered by the
`store_subview_t`.) */

class backfiller_t : public home_thread_mixin_debug_only_t {
public:
    backfiller_t(mailbox_manager_t *_mailbox_manager,
                 branch_history_manager_t *_branch_history_manager,
                 store_view_t *_store);
    ~backfiller_t();

    backfiller_bcard_t get_business_card() const;

private:
    /* A `client_t` is created for every backfill that's in progress. */
    class client_t;

    mailbox_manager_t *const mailbox_manager;
    branch_history_manager_t *const branch_history_manager;
    store_view_t *const store;

    registrar_t<backfiller_bcard_t::intro_1_t, backfiller_t *, client_t> registrar;

    DISABLE_COPYING(backfiller_t);
};

#endif /* CLUSTERING_IMMEDIATE_CONSISTENCY_BACKFILLER_HPP_ */
