// The search session's one instance (SearchSession.hpp). A file static rather
// than an inline variable, so nothing reaches it except through searchSession().
#include "SearchSession.hpp"

static Search::CSession g_search;

Search::CSession& searchSession() {
    return g_search;
}
