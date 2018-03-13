#ifndef PERFMON_CTOR_COUNTER_HPP_
#define PERFMON_CTOR_COUNTER_HPP_

#include <string>
#include <typeinfo>

#include "perfmon/perfmon.hpp"

namespace perfmon {

// Usage: class T : ctor_counter<T> { ... }.  The T template parameter distinguishes the
// static variables used to count constructions/destructions.
template <class T>
class ctor_counter {
public:
    ctor_counter() {
        ++cs_.create_count;
    }
    ~ctor_counter() {
        ++cs_.destroy_count;
    }

    // We put the four static fields in a single struct so that they _all_ the get
    // instantiated when one gets used.  (If they were four separate naked static
    // fields, only the perfmon_counter_t's would get template-instantiated because
    // nothing directly uses the perfmon_membership_t's.)
    struct counters {
        perfmon_counter_t create_count;
        perfmon_counter_t destroy_count;

        perfmon_membership_t create_membership;
        perfmon_membership_t destroy_membership;

        counters()
            : create_count(), destroy_count(),
              create_membership(&get_global_perfmon_collection(), &create_count, std::string("create_") + typeid(T).name(), false),
              destroy_membership(&get_global_perfmon_collection(), &destroy_count, std::string("destroy_") + typeid(T).name(), false) {}
    };

private:
    static counters cs_;
};

template <class T>
typename ctor_counter<T>::counters ctor_counter<T>::cs_;

}  // namespace perfmon

#endif  // PERFMON_CTOR_COUNTER_HPP_
