#include "graph.hh"

#include <algorithm>
#include <numeric>
#include <chrono>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <map>
#include <list>
#include <cassert>
#include <functional>
#include <array>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "mcsp.h"

using std::vector;
using std::cout;
using std::endl;

using std::chrono::steady_clock;
using std::chrono::milliseconds;
using std::chrono::duration_cast;

using namespace std;

static void fail(const std::string& msg) {
    std::cerr << msg << std::endl;
    exit(1);
}

#define MAX_ARGS 10
#define SORTED -1
#define OSCILLATING 0

/*******************************************************************************
                             Command-line arguments
*******************************************************************************/

static std::atomic<bool> abort_due_to_timeout;
atomic<int> global_position{ 0 };

struct args arguments;

/*******************************************************************************
                               Data structures
*******************************************************************************/

struct VtxSet {
    int vv[MAX_ARGS] = {};
    VtxSet(const int *v) {
        for(int i=0; i<arguments.arg_num; i++) {
            vv[i] = v[i];
        }
    }
};

struct Multidomain {
    int sets[MAX_ARGS] = {};
    int len[MAX_ARGS] = {};
    bool is_adjacent;

    Multidomain(const int *sets, const int *len, const bool adj)  {
        for (int i=0; i< arguments.arg_num; i++){
            this->sets[i] = sets[i];
            this->len[i] = len[i];
        }
        is_adjacent = adj;
    }
};

struct AtomicIncumbent
{
    std::atomic<unsigned> value;

    AtomicIncumbent()
    {
        value.store(0, std::memory_order_seq_cst);
    }

    bool update(const unsigned v)
    {
        while (true) {
            unsigned cur_v = value.load(std::memory_order_seq_cst);
            if (v > cur_v) {
                if (value.compare_exchange_strong(cur_v, v, std::memory_order_seq_cst)) {
                    return true;
                }
            }
            else
                return false;
        }
    }
};

using PerThreadIncumbents = std::map<std::thread::id, vector<VtxSet> >;

const constexpr int split_levels = 4;

struct Position
{
    std::array<unsigned, split_levels + 1> values = {};
    unsigned depth;

    Position()
    {
        std::fill(values.begin(), values.end(), 0);
        depth = 0;
    }

    bool operator< (const Position & other) const
    {
        if (depth < other.depth)
            return true;
        else if (depth > other.depth)
            return false;

        for (unsigned p = 0 ; p < split_levels + 1 ; ++p)
            if (values.at(p) < other.values.at(p))
                return true;
            else if (values.at(p) > other.values.at(p))
                return false;

        return false;
    }

    void add(const unsigned d, const unsigned v)
    {
        depth = d;
        if (d <= split_levels)
            values[d] = v;
    }
};

/*******************************************************************************
                                    Utils
*******************************************************************************/

void show(const vector<VtxSet>& current, const vector<Multidomain>& domains, const array<vector<int>, MAX_ARGS>& vv) {
    cout << "Length of current assignment: " << current.size() << endl;
    cout << "Current assignment:";

    for (const auto &c : current) {
        for (int j = 0; j < arguments.arg_num; j++) {
            if (j == 0)
                cout << "  " << c.vv[j];
            else
                cout << "->" << c.vv[j];
        }
    }
    cout << endl;
    for (const auto& bd : domains) {
        for (int ng = 0; ng < arguments.arg_num; ng++) {
            cout << "Graph " << ng << "  ";
            for (int j = 0; j < bd.len[ng]; j++)
                cout << vv[ng][j+bd.sets[ng]] << " ";
            cout << endl;
        }
    }
    cout << endl << endl;
}

void string_show(const vector<VtxSet>& current, const int depth) {
    string s;

    for (const auto & c : current) {
        for (int j = 0; j < arguments.arg_num; j++) {
            if (j == 0) {
                s += to_string(c.vv[j]);
            }
            else {
                s += "->" + to_string(c.vv[j]);
            }
        }
        s += " ";
    }
    s += ": " + to_string(depth);
    s += "\n";
    cout << s;
}

/*******************************************************************************
                                Parallel queue
*******************************************************************************/

struct HelpMe
{
    struct Task
    {
        const std::function<void (unsigned long long &)> * func;
        int pending;
    };

    std::mutex general_mutex;
    std::condition_variable cv;
    std::map<Position, Task> tasks;
    std::atomic<bool> finish;

    vector<std::thread> threads;

    std::list<milliseconds> times;
    std::list<unsigned long long> nodes;

    HelpMe(int n_threads) :
        finish(false)
    {
        for (int t = 0 ; t < n_threads ; ++t)
            threads.emplace_back([this] {
                    milliseconds total_work_time = milliseconds::zero();
                    unsigned long long this_thread_nodes = 0;
                    while (! finish.load()) {
                        std::unique_lock<std::mutex> guard(general_mutex);
                        bool did_something = false;
                        for (auto task = tasks.begin() ; task != tasks.end() ; ++task) {
                            if (task->second.func) {
                                const auto f = task->second.func;
                                ++task->second.pending;
                                guard.unlock();

                                auto start_work_time = steady_clock::now(); // local start time

                                (*f)(this_thread_nodes);

                                const auto work_time = duration_cast<milliseconds>(steady_clock::now() - start_work_time);
                                total_work_time += work_time;

                                guard.lock();
                                task->second.func = nullptr;
                                if (0 == --task->second.pending)
                                    cv.notify_all();

                                did_something = true;
                                break;
                            }
                        }

                        if ((! did_something) && (! finish.load()))
                            cv.wait(guard);
                    }

                    std::unique_lock<std::mutex> guard(general_mutex);
                    times.push_back(total_work_time);
                    nodes.push_back(this_thread_nodes);
                    });
    }

    auto kill_workers() -> void
    {
        {
            std::unique_lock<std::mutex> guard(general_mutex);
            finish.store(true);
            cv.notify_all();
        }

        for (auto & t : threads)
            t.join();

        threads.clear();

        if (! times.empty()) {
            cout << "Thread work times";
            for (auto & t : times)
                cout << " " << t.count();
            cout << endl;
            times.clear();
        }
    }

    ~HelpMe()
    {
        kill_workers();
    }

    HelpMe(const HelpMe &) = delete;

    void get_help_with(
            const Position & position,
            const std::function<void (unsigned long long &)> & main_func,
            const std::function<void (unsigned long long &)> & thread_func,
            unsigned long long & main_nodes)
    {
        std::map<Position, HelpMe::Task>::iterator task;
        {
            std::unique_lock<std::mutex> guard(general_mutex);
            const auto r = tasks.emplace(position, HelpMe::Task{ &thread_func, 0 });
            assert(r.second);
            task = r.first;
            cv.notify_all();
        }

        main_func(main_nodes);

        {
            std::unique_lock<std::mutex> guard(general_mutex);
            while (0 != task->second.pending)
                cv.wait(guard);
            tasks.erase(task);
        }
    }
};

/*******************************************************************************
                                 MCS functions
*******************************************************************************/

inline bool check_sol(const vector<Graph> & g, const vector<VtxSet> & solution) {

    for (size_t i = 0; i < solution.size(); i++) {
        for (size_t ng = 1; ng < arguments.arg_num; ng++) {
            if(g[0].label[solution[i].vv[0]] != g[ng].label[solution[i].vv[ng]]) {
                return false;
            }
            for (size_t j = i+1; j < solution.size(); j++) {
                if(g[0].adjmat[solution[i].vv[0]][solution[j].vv[0]] != g[ng].adjmat[solution[i].vv[ng]][solution[j].vv[ng]]) {
                    return false;
                }
            }
        }
    }
    return true;
}

inline int calc_bound(const vector<Multidomain>& domains) {
    int bound = 0;
    for (const Multidomain &bd : domains) {
        bound += *min_element(bd.len, bd.len+arguments.arg_num);
    }
    return bound;
}

inline int find_min_value(const int *arr, const int start_idx, const int len) {
    int min_v = INT_MAX;
    for (int i=0; i<len; i++)
        if (arr[start_idx + i] < min_v)
            min_v = arr[start_idx + i];
    return min_v;
}

inline int select_multidomain(const vector<Multidomain>& domains, const int* left,
        const int current_matching_size)
{
    // Select the bidomain with the smallest max(leftsize, rightsize), breaking
    // ties on the smallest vertex index in the left set
    int min_size = INT_MAX;
    int min_tie_breaker = INT_MAX;
    int best = -1;
    for (unsigned int i=0; i<domains.size(); i++) {
        const Multidomain &bd = domains[i];
        if (arguments.connected && current_matching_size>0 && !bd.is_adjacent) continue;
        int len;
        switch (arguments.heuristic) {
            case min_max:
                len = *max_element(bd.len, bd.len+arguments.arg_num);
            break;
            case min_min:
                len = *min_element(bd.len, bd.len+arguments.arg_num);
            break;
            case min_sum:
                len = accumulate(bd.len, bd.len+arguments.arg_num, 0);
            break;
            case min_product:
                len = accumulate(bd.len, bd.len+arguments.arg_num, 1, std::multiplies<>());
            break;
            default:
                cout << "Error, not implemented heuristic!" << endl;
                exit (-1);
        }
        if (len < min_size) {
            min_size = len;
            min_tie_breaker = find_min_value(left, bd.sets[0], bd.len[0]);
            best = i;
        } else if (len == min_size) {
            const int tie_breaker = find_min_value(left, bd.sets[0], bd.len[0]);
            if (tie_breaker < min_tie_breaker) {
                min_tie_breaker = tie_breaker;
                best = i;
            }
        }
    }
    return best;
}

// Returns length of left half of array
inline int partition(int *all_vv, const int start, const int len, const vector<unsigned int> & adjrow) {
    int i=0;
    for (int j=0; j<len; j++) {
        if (adjrow[all_vv[start+j]]) {
            std::swap(all_vv[start+i], all_vv[start+j]);
            i++;
        }
    }
    return i;
}

inline bool check_greater(const int *lower, const int *greater) {
    bool ret_val = true;
    for (int i = 0; i < arguments.arg_num; i++) {
        ret_val &= greater[i] > lower[i];
    }
    return ret_val;
}

inline unsigned int min_elem(const vector<unsigned int>& vet) {
    unsigned int min = UINT_MAX;
    for (int i = 0; i < arguments.arg_num; i++) {
        if (vet[i] < min) {
            min = vet[i];
        }
    }
    return min;
}

inline int max_elem(const vector<unsigned int>& vet) {
    int max = 0;
    int counter = 0;
    for (int i = 0; i < arguments.arg_num; i++) {
        if (vet[i] > max) {
            max = vet[i];
            counter = 1;
        }
        else if (vet[i] == max) {
            counter++;
        }
    }
    if (counter == arguments.arg_num) {
        return -1;
    }
    return max;
}

// multiway is for directed and/or labelled graphs
inline vector<Multidomain> filter_domains(const vector<Multidomain>& d, array<vector<int>, MAX_ARGS> &vv,
    const vector<Graph>& g, const int *vertex, const bool multiway)
{
    vector<Multidomain> new_d;
    new_d.reserve(d.size());
    for (const Multidomain& old_bd : d) {
        int sets[MAX_ARGS] = {};
        for(int i=0; i<arguments.arg_num; i++) {
            sets[i] = old_bd.sets [i];
        }
        //vector<int> sets = old_bd.sets; /*l, r*/
        // After these two partitions, left_len and right_len are the lengths of the
        // arrays of vertices with edges from v or w (int the directed case, edges
        // either from or to v or w)
        int len_edge[MAX_ARGS] = {}; /*left_len, right_len*/
        int len_noedge[MAX_ARGS] = {}; /*left_len_noedge, right_len_noedge*/
        for (int i = 0; i < arguments.arg_num; i++) {
            len_edge[i] = partition(vv[i].data(), sets[i], old_bd.len[i], g[i].adjmat[vertex[i]]);
            len_noedge[i] = old_bd.len[i] - len_edge[i];
        }

        if (accumulate(len_noedge, len_noedge+arguments.arg_num, 1, multiplies<>{})) {
            vector<int> new_vector_sets(arguments.arg_num);
            int *new_d_sets = new_vector_sets.data();
            transform(len_edge, len_edge+arguments.arg_num, sets, new_d_sets, std::plus<>());
            new_d.emplace_back(Multidomain(new_d_sets, len_noedge, old_bd.is_adjacent));
        }
        bool is_empty = accumulate(len_edge, len_edge+arguments.arg_num, 1, multiplies<>{});
        if (multiway && is_empty) {
            vector<const vector<unsigned int> *> adjrows(arguments.arg_num);
            int top[MAX_ARGS];
            for (int i = 0; i < arguments.arg_num; i++) {
                adjrows[i] = &g[i].adjmat[vertex[i]];
                int* _begin = vv[i].data() + sets[i];
                std::sort(_begin, _begin + len_edge[i], [&](const int a, const int b)
                    { return adjrows.at(i)->at(a) < adjrows.at(i)->at(b); } );
                top[i] = sets[i] + len_edge[i];
            }
            while (check_greater(sets, top)) {
                vector<unsigned int> labels(arguments.arg_num);
                for (int i = 0; i < arguments.arg_num; i++) {
                    labels[i] = adjrows.at(i)->at(vv[i][sets[i]]);
                }
                const int maximum = max_elem(labels);
                if (maximum != -1) {
                    for (int i = 0; i < arguments.arg_num; i++) {
                        if (labels[i] != maximum) {
                            sets[i]++;
                        }
                    }
                }
                else {
                    int min_sets[MAX_ARGS]= {};
                    for (int  i=0; i<arguments.arg_num; i++) {
                        min_sets[i] = sets[i];
                    }
                    for (int i = 0; i < arguments.arg_num; i++) {
                        do { sets[i]++; } while (sets[i] < top[i] && adjrows.at(i)->at(vv[i][sets[i]]) == labels[0]);
                    }
                    int dif_sets[MAX_ARGS];
                    transform(sets, sets+arguments.arg_num, min_sets, dif_sets, std::minus<>());
                    new_d.emplace_back(Multidomain(min_sets, dif_sets, true));
                }
            }
        }
        else if (is_empty) {
            new_d.emplace_back(Multidomain(sets, len_edge, true));
        }
    }
    return new_d;
}

// returns the index of the smallest value in arr that is >w.
// Assumption: such a value exists
// Assumption: arr contains no duplicates
// Assumption: arr has no values==INT_MAX
inline int index_of_next_smallest(const int *arr, const int start_idx, const int len, const int w) {
    int idx = -1;
    int smallest = INT_MAX;
    for (int i=0; i<len; i++) {
        if (arr[start_idx + i]>w && arr[start_idx + i]<smallest) {
            smallest = arr[start_idx + i];
            idx = i;
        }
    }
    return idx;
}

inline void remove_vtx_from_domain(int *left, Multidomain& bd, int v, int idx)
{
    int i = 0;
    while(left[bd.sets[idx] + i] != v) i++;
    std::swap(left[bd.sets[idx]+i], left[bd.sets[idx]+bd.len[idx]-1]);
    bd.len[idx]--;
}

inline void remove_bidomain(vector<Multidomain>& domains, const int idx) {
    domains[idx] = domains[domains.size()-1];
    domains.pop_back();
}

inline void solve_first_graph(array<vector<int>, MAX_ARGS>& vv,
    array<int, MAX_ARGS>& nodi_inseriti,
    const array<int, MAX_ARGS> order, Multidomain& bd)
{

    const int pos = order[0];
    // reduce by 1 the size of each domain not in the first graph (assuming we are selecting a node)
    for (int i = 1; i < arguments.arg_num; i++) {
        bd.len[order[i]]--;
    }

    // select smallest node as v
    const int v = find_min_value(vv[pos].data(), bd.sets[pos], bd.len[pos]);
    // remove v from domain, either it is in the solution or nowhere
    remove_vtx_from_domain(vv[pos].data(), bd, v, pos);

    // add v into solution
    nodi_inseriti[pos] = v;
    
    // new node, let's explore it
}

inline bool solve_other_graphs(array<vector<int>, MAX_ARGS>& vv,
    const int pos, const Multidomain& bd,
    int& w)
{

    // let's select the smallest
    const int idx = index_of_next_smallest(vv[pos].data(), bd.sets[pos], bd.len[pos] + 1, w);
    if (idx == -1) {
        return false; // no more w in domain
    }
    w = vv[pos][bd.sets[pos] + idx];

    // move w at the end of domain
    vv[pos][bd.sets[pos] + idx] = vv[pos][bd.sets[pos] + bd.len[pos]];
    vv[pos][bd.sets[pos] + bd.len[pos]] = w;

    return true; // new w, explore it
}

void sorted_solve_nopar(const unsigned depth, vector<Graph> & g,
    AtomicIncumbent & global_incumbent,
    vector<VtxSet> & my_incumbent,
    vector<VtxSet> & current, vector<Multidomain> & domains,
    array<vector<int>, MAX_ARGS> &vv,
    const unsigned int matching_size_goal,
    unsigned long long & my_thread_nodes)
{

    if (my_incumbent.size() < current.size()) {
        my_incumbent = current;
        global_incumbent.update(current.size());
    }

    if (arguments.verbose) {
        //show(current, domains, vv);
        string_show(current, depth);
    }

    my_thread_nodes++;

    const unsigned int bound = current.size() + calc_bound(domains);
    if (bound <= global_incumbent.value || bound < matching_size_goal)
        return;

    if (arguments.big_first && global_incumbent.value == matching_size_goal)
        return;

    
    // select a multidomain
    int bd_idx = select_multidomain(domains, vv[0].data(), current.size());
    if (bd_idx == -1)
        return; // nothing more to do

    auto &bd = domains[bd_idx];

#if (SORTED == 1)
#if OSCILLATING
    array<int, MAX_ARGS> sorted_vv_idx = {};
    array<int, MAX_ARGS> tmp_sorted_vv_idx = {};
    iota(tmp_sorted_vv_idx.begin(), tmp_sorted_vv_idx.begin() + MAX_ARGS, 0);
    // let's sort
    stable_sort(tmp_sorted_vv_idx.begin(), tmp_sorted_vv_idx.begin() + arguments.arg_num,
        [&](const int a, const int b) {
            return (bd.len[a] < bd.len[b]);
        }
    );
    for (int i = 0; i < arguments.arg_num; i = i + 2) {
        sorted_vv_idx[i] = tmp_sorted_vv_idx[i / 2];
    }
    for (int i = 1; i < arguments.arg_num; i = i + 2) {
        sorted_vv_idx[i] = tmp_sorted_vv_idx[arguments.arg_num - 1 - i / 2];
    }
#else
    array<int, MAX_ARGS> sorted_vv_idx = {};
    iota(sorted_vv_idx.begin(), sorted_vv_idx.begin() + MAX_ARGS, 0);
    // let's sort
    stable_sort(sorted_vv_idx.begin(), sorted_vv_idx.begin() + arguments.arg_num,
        [&](const int a, const int b) {
            return (bd.len[a] < bd.len[b]);
        }
    );
#endif
#elif (SORTED == -1)
    array<int, MAX_ARGS> sorted_vv_idx = {};
    iota(sorted_vv_idx.begin(), sorted_vv_idx.begin() + MAX_ARGS, 0);
    // let's sort
    stable_sort(sorted_vv_idx.begin(), sorted_vv_idx.begin() + arguments.arg_num,
        [&](const int a, const int b) {
            return (bd.len[a] >= bd.len[b]);
        }
    );
#else
    array<int, MAX_ARGS> sorted_vv_idx = {};
    iota(sorted_vv_idx.begin(), sorted_vv_idx.begin() + MAX_ARGS, 0);
#endif

    array<int, MAX_ARGS> soluzione = {};
    for (int i = 0; i < MAX_ARGS; i++) { soluzione[i] = -1; }

    solve_first_graph(vv, soluzione, sorted_vv_idx, bd);

    for (int i = 1; i > 0; ) {
        if (solve_other_graphs(vv, sorted_vv_idx[i], bd, soluzione[sorted_vv_idx[i]]))
        {
            i ++;
            if (i == arguments.arg_num) {
                current.emplace_back(VtxSet(soluzione.data()));
                auto new_domains = filter_domains(domains, vv, g, soluzione.data(), arguments.directed || arguments.edge_labelled);
                if (abort_due_to_timeout)
                    return;
                sorted_solve_nopar(depth + 1, g, global_incumbent, my_incumbent, current, new_domains, vv, matching_size_goal, my_thread_nodes);
                i --;
                current.pop_back();
            }
        }
        else
        {
            soluzione[sorted_vv_idx[i]] = -1;
            i --;
        }
    }

    if (bd.len[sorted_vv_idx[0]] == 0)
    {
        remove_bidomain(domains, bd_idx);
    }
    else 
    {
        for (int i = 1; i < arguments.arg_num; i++) {
            bd.len[sorted_vv_idx[i]] ++;
        }
    }
    
    // let's pair first node with empty and keep going
    sorted_solve_nopar(depth + 1, g, global_incumbent, my_incumbent, current, domains, vv, matching_size_goal, my_thread_nodes);
}

void sorted_solve(const unsigned depth, vector<Graph>& g,
    AtomicIncumbent& global_incumbent,
    PerThreadIncumbents& per_thread_incumbents,
    vector<VtxSet>& current, vector<Multidomain>& domains,
    array<vector<int>, MAX_ARGS>& vv,
    const unsigned int matching_size_goal,
    const Position& position, HelpMe& help_me,
    unsigned long long& my_thread_nodes)
{


    if (per_thread_incumbents.find(std::this_thread::get_id())->second.size() < current.size()) {
        per_thread_incumbents.find(std::this_thread::get_id())->second = current;
        global_incumbent.update(current.size());
    }

    if (arguments.verbose) {
        string_show(current, depth);
    }

    my_thread_nodes++;

    const unsigned int bound = current.size() + calc_bound(domains);
    if (bound <= global_incumbent.value || bound < matching_size_goal)
        return;

    if (arguments.big_first && global_incumbent.value == matching_size_goal)
        return;

    int bd_idx = select_multidomain(domains, vv[0].data(), current.size());
    if (bd_idx == -1)   // In the MCCS case, there may be nothing we can branch on
        return;
    Multidomain& bd = domains[bd_idx];

#if (SORTED == 1)
#if OSCILLATING
    array<int, MAX_ARGS> sorted_vv_idx = {};
    array<int, MAX_ARGS> tmp_sorted_vv_idx = {};
    iota(tmp_sorted_vv_idx.begin(), tmp_sorted_vv_idx.begin() + MAX_ARGS, 0);
    // let's sort
    stable_sort(tmp_sorted_vv_idx.begin(), tmp_sorted_vv_idx.begin() + arguments.arg_num,
        [&](const int a, const int b) {
            return (bd.len[a] < bd.len[b]);
        }
    );
    for (int i = 0; i < arguments.arg_num; i = i + 2) {
        sorted_vv_idx[i] = tmp_sorted_vv_idx[i / 2];
    }
    for (int i = 1; i < arguments.arg_num; i = i + 2) {
        sorted_vv_idx[i] = tmp_sorted_vv_idx[arguments.arg_num - 1 - i / 2];
    }
#else
    array<int, MAX_ARGS> sorted_vv_idx = {};
    iota(sorted_vv_idx.begin(), sorted_vv_idx.begin() + MAX_ARGS, 0);
    // let's sort
    stable_sort(sorted_vv_idx.begin(), sorted_vv_idx.begin() + arguments.arg_num,
        [&](const int a, const int b) {
            return (bd.len[a] < bd.len[b]);
        }
    );
#endif
#elif (SORTED == -1)
    array<int, MAX_ARGS> sorted_vv_idx = {};
    iota(sorted_vv_idx.begin(), sorted_vv_idx.begin() + MAX_ARGS, 0);
    // let's sort
    stable_sort(sorted_vv_idx.begin(), sorted_vv_idx.begin() + arguments.arg_num,
        [&](const int a, const int b) {
            return (bd.len[a] >= bd.len[b]);
        }
    );
#else
    array<int, MAX_ARGS> sorted_vv_idx = {};
    iota(sorted_vv_idx.begin(), sorted_vv_idx.begin() + MAX_ARGS, 0);
#endif

    array<int, MAX_ARGS> soluzione = {};
    for (int i = 0; i < MAX_ARGS; i++) { soluzione[i] = -1; }

    solve_first_graph(vv, soluzione, sorted_vv_idx, bd);

    std::atomic<int> shared_i{ 0 };
    const int i_end = bd.len[sorted_vv_idx[1]] + 2; /* including the null */

    // Version of the loop used by helpers
    const std::function<void(unsigned long long&)> helper_function =
        [&shared_i, &g, &global_incumbent, &per_thread_incumbents, &position,
        &depth, i_end, matching_size_goal, &help_me, current, domains, vv, bd_idx, sorted_vv_idx, soluzione]
        (unsigned long long& help_thread_nodes) 
    {

        int which_i_should_i_run_next = shared_i++;

        if (which_i_should_i_run_next >= i_end)
            return; /* don't waste time recomputing */
        
        vector<VtxSet> help_current = current;
        vector<Multidomain> help_domains = domains;
        array<vector<int>, MAX_ARGS> help_vv = vv;
        array<int, MAX_ARGS> help_soluzione = soluzione;

        Multidomain& bd = help_domains[bd_idx];

        int w0_index = 0; //used to check if w has already been tested before

        // search other graphs
        for (int i = 1; i > 0; ) {
            if (solve_other_graphs(help_vv, sorted_vv_idx[i], bd, help_soluzione[sorted_vv_idx[i]]))
            {
                const int prev_i = i;
                const bool should_i = (i != 1 || which_i_should_i_run_next == w0_index);
                if (should_i) {
                    i++;
                    if (i == arguments.arg_num) {
                        help_current.emplace_back(VtxSet(help_soluzione.data()));
                        auto new_domains = filter_domains(help_domains, help_vv, g, help_soluzione.data(), arguments.directed || arguments.edge_labelled);
                        if (depth > split_levels) {
                            if (abort_due_to_timeout)
                                return;
                            sorted_solve_nopar(depth + 1, g, global_incumbent, per_thread_incumbents.find(this_thread::get_id())->second, help_current, new_domains, help_vv, matching_size_goal, help_thread_nodes);
                        }
                        else {
                            auto new_position = position;
                            new_position.add(depth, ++global_position);
                            if (abort_due_to_timeout)
                                return;
                            sorted_solve(depth + 1, g, global_incumbent, per_thread_incumbents, help_current, new_domains, help_vv, matching_size_goal, new_position, help_me, help_thread_nodes);
                        }
                        i--;
                        help_current.pop_back();
                    }
                    if (prev_i == 1) {
                        which_i_should_i_run_next = shared_i++;
                    }
                }
                w0_index += (prev_i == 1);
            }
            else
            {
                help_soluzione[sorted_vv_idx[i]] = -1;
                i--;
            }
        }

        if (bd.len[sorted_vv_idx[0]] == 0)
        {
            remove_bidomain(help_domains, bd_idx);
        }
        else
        {
            for (int i = 1; i < arguments.arg_num; i++) {
                bd.len[sorted_vv_idx[i]] ++;
            }
        }

        // let's pair first node with empty and keep going
        if (which_i_should_i_run_next == w0_index) {
            if (depth > split_levels) {
                sorted_solve_nopar(depth + 1, g, global_incumbent, per_thread_incumbents.find(this_thread::get_id())->second, help_current, help_domains, help_vv, matching_size_goal, help_thread_nodes);
            }
            else {
                auto new_position = position;
                new_position.add(depth, ++global_position);
                sorted_solve(depth + 1, g, global_incumbent, per_thread_incumbents, help_current, help_domains, help_vv, matching_size_goal, new_position, help_me, help_thread_nodes);
            }
        }

    };

    // Grab this first, before advertising that we can get help
    int which_i_should_i_run_next = shared_i++;

    // Version of the loop used by the main thread
    const std::function<void(unsigned long long&)> main_function = [&](unsigned long long& main_thread_nodes) {

        int w0_index = 0;

        for (int i = 1; i > 0; ) {
            if (solve_other_graphs(vv, sorted_vv_idx[i], bd, soluzione[sorted_vv_idx[i]]))
            {
                const int prev_i = i;
                const bool should_i = (i != 1 || which_i_should_i_run_next == w0_index);
                if (should_i) {
                    i++;
                    if (i == arguments.arg_num) {
                        current.emplace_back(VtxSet(soluzione.data()));
                        auto new_domains = filter_domains(domains, vv, g, soluzione.data(), arguments.directed || arguments.edge_labelled);
                        if (depth > split_levels) {
                            if (abort_due_to_timeout)
                                return;
                            sorted_solve_nopar(depth + 1, g, global_incumbent, per_thread_incumbents.find(this_thread::get_id())->second, current, new_domains, vv, matching_size_goal, my_thread_nodes);
                        }
                        else {
                            auto new_position = position;
                            new_position.add(depth, ++global_position);
                            if (abort_due_to_timeout)
                                return;
                            sorted_solve(depth + 1, g, global_incumbent, per_thread_incumbents, current, new_domains, vv, matching_size_goal, new_position, help_me, my_thread_nodes);
                        }
                        i--;
                        current.pop_back();
                    }
                    if (prev_i == 1) {
                        which_i_should_i_run_next = shared_i++;
                    }
                }
                w0_index += (prev_i == 1);
            }
            else
            {
                soluzione[sorted_vv_idx[i]] = -1;
                i--;
            }
        }

        if (bd.len[sorted_vv_idx[0]] == 0)
        {
            remove_bidomain(domains, bd_idx);
        }
        else
        {
            for (int i = 1; i < arguments.arg_num; i++) {
                bd.len[sorted_vv_idx[i]] ++;
            }
        }

        // let's pair first node with empty and keep going
        if (which_i_should_i_run_next == w0_index) {
            if (depth > split_levels) {
                sorted_solve_nopar(depth + 1, g, global_incumbent, per_thread_incumbents.find(this_thread::get_id())->second, current, domains, vv, matching_size_goal, my_thread_nodes);
            }
            else {
                auto new_position = position;
                new_position.add(depth, ++global_position);
                sorted_solve(depth + 1, g, global_incumbent, per_thread_incumbents, current, domains, vv, matching_size_goal, new_position, help_me, my_thread_nodes);
            }
        }
    };

    if (depth <= split_levels) {
        help_me.get_help_with(position, main_function, helper_function, my_thread_nodes);
    }
    else {
        main_function(my_thread_nodes);
    }

}

std::set<unsigned int> intersection(const vector<set<unsigned int>>& vecs) {

    auto last_intersection = vecs[0];
    set<unsigned int> curr_intersection;

    for (std::size_t i = 1; i < vecs.size(); ++i) {
        std::set_intersection(last_intersection.begin(), last_intersection.end(),
            vecs[i].begin(), vecs[i].end(),
            std::inserter(curr_intersection, std::begin(curr_intersection)));
        std::swap(last_intersection, curr_intersection);
        curr_intersection.clear();
    }
    return last_intersection;
}

std::pair<vector<VtxSet>, unsigned long long> mcs(vector<Graph> & gi) {
    
    // the buffer of vertex indices for the partitions
    array<vector<int>, MAX_ARGS> vtx_buf;

    auto domains = vector<Multidomain>{};

    vector<set<unsigned int>> labels_vv (arguments.arg_num);
    for (int i = 0; i < arguments.arg_num; i++) {
        for (unsigned int label : gi[i].label) {
            labels_vv[i].insert(label);
        }
    }

    std::set<unsigned int> labels = intersection(labels_vv);
    
    // Create a bidomain for each label that appears in both graphs
    for (unsigned int label : labels) {
        int starts[MAX_ARGS] = {};
        int len[MAX_ARGS] = {};
        for (int i = 0; i < arguments.arg_num; i++) {
            int jj = 0;
            starts[i] = vtx_buf[i].size();
            for (int j = 0; j < gi[i].n; j++) {
                if (gi[i].label[j] == label) {
                    vtx_buf[i].push_back(j);
                }
            }
            len[i] = vtx_buf[i].size() - starts[i];
        }

        domains.emplace_back(Multidomain(starts, len, false));
    }

    AtomicIncumbent global_incumbent;
    vector<VtxSet> incumbent;
    unsigned long long global_nodes = 0;

    if (arguments.big_first) {
        for (int k=0; k<gi[0].n; k++) {
            unsigned int goal = gi[0].n - k;
            array<vector<int>, MAX_ARGS> vtx_buf_copy;
            for (int i=0; i<arguments.arg_num; i++) {
                vtx_buf_copy[i].reserve(vtx_buf[i].size());
                for (unsigned int j=0; j<vtx_buf[i].size(); j++) {
                    vtx_buf_copy[i].push_back(vtx_buf[i][j]);
                }
            }
            auto domains_copy = domains;
            vector<VtxSet> current;
            PerThreadIncumbents per_thread_incumbents;
            per_thread_incumbents.emplace(std::this_thread::get_id(), vector<VtxSet>());
            Position position;
            HelpMe help_me(arguments.threads - 1);
            for (auto & t : help_me.threads)
                per_thread_incumbents.emplace(t.get_id(), vector<VtxSet>());

            sorted_solve(0, gi, global_incumbent, per_thread_incumbents, current, domains_copy, vtx_buf_copy, goal, position, help_me, global_nodes);

            help_me.kill_workers();
            for (auto & n : help_me.nodes) {
                global_nodes += n;
            }
            for (auto & i : per_thread_incumbents)
                if (i.second.size() > incumbent.size())
                    incumbent = i.second;
            if (global_incumbent.value == goal || abort_due_to_timeout) break;
            if (!arguments.quiet) cout << "Upper bound: " << goal-1 << std::endl;
        }

    } else {
        vector<VtxSet> current;
        PerThreadIncumbents per_thread_incumbents;
        per_thread_incumbents.emplace(std::this_thread::get_id(), vector<VtxSet>());
        Position position;
        HelpMe help_me(arguments.threads - 1);
        for (auto & t : help_me.threads)
            per_thread_incumbents.emplace(t.get_id(), vector<VtxSet>());
            
        sorted_solve(0, gi, global_incumbent, per_thread_incumbents, current, domains, vtx_buf, 1, position, help_me, global_nodes);
        
        help_me.kill_workers();
        for (auto & n : help_me.nodes)
            global_nodes += n;
        for (auto & i : per_thread_incumbents)
            if (i.second.size() > incumbent.size())
                incumbent = i.second;
    }

    return { incumbent, global_nodes };
}

vector<int> calculate_degrees(const Graph & g) {
    vector<int> degree(g.n, 0);
    for (int v=0; v<g.n; v++) {
        for (int w=0; w<g.n; w++) {
            unsigned int mask = 0xFFFFu;
            if (g.adjmat[v][w] & mask) degree[v]++;
            if (g.adjmat[v][w] & ~mask) degree[v]++;  // inward edge, in directed case
        }
    }
    return degree;
}

int sum(const vector<int> & vec) {
    return std::accumulate(std::begin(vec), std::end(vec), 0);
}

void mcsp::start(const args &arg) {
    arguments = arg;

    const char format = arguments.dimacs ? 'D' : arguments.lad ? 'L' : arguments.bin_enrico ? 'E' : arguments.ioi ? 'I' : 'B';
    vector<Graph> gi;
    for (int i = 0; i < arguments.arg_num; i++) {
        gi.push_back(readGraph(arguments.filenames[i], format, arguments.directed,
            arguments.edge_labelled, arguments.vertex_labelled));
    }

    std::thread timeout_thread;
    std::mutex timeout_mutex;
    std::condition_variable timeout_cv;
    abort_due_to_timeout.store(false);
    bool aborted = false;

    if (0 != arguments.timeout) {
        timeout_thread = std::thread(
        [&] {
	            const auto abort_time = steady_clock::now() + std::chrono::seconds(arguments.timeout);
	            {
	                /* Sleep until either we've reached the time limit,
	                 * or we've finished all the work. */
	                std::unique_lock<std::mutex> guard(timeout_mutex);
	                while (! abort_due_to_timeout.load()) {
	                    if (std::cv_status::timeout == timeout_cv.wait_until(guard, abort_time)) {
	                        /* We've woken up, and it's due to a timeout. */
	                        aborted = true;
	                        break;
	                    }
	                }
	            }
	            abort_due_to_timeout.store(true);
            }
        );
    }

    const double begin = clock ();
    const auto start = steady_clock::now();
    
    vector<vector<int>> gi_deg(arguments.arg_num);
    for (int i = 0; i < arguments.arg_num; i++) {
        gi_deg[i] = calculate_degrees(gi[i]);
    }

    // As implemented here, g1_dense and g0_dense are false for all instances
    // in the Experimental Evaluation section of the paper.  Thus,
    // we always sort the vertices in descending order of degree (or total degree,
    // in the case of directed graphs.  Improvements could be made here: it would
    // be nice if the program explored exactly the same search tree if both
    // input graphs were complemented.
    vector<vector<int>> vvi(arguments.arg_num);
    for (int i = 0; i < arguments.arg_num; i++) {
        vvi[i].resize(gi[i].n);
        iota(std::begin(vvi[i]), std::end(vvi[i]), 0);
        stable_sort(std::begin(vvi[i]), std::end(vvi[i]), [&](int a, int b) {
            return (gi_deg[i][a] > gi_deg[i][b]);
        });
    }

    vector<Graph> gi_sorted;
    gi_sorted.reserve(MAX_ARGS);
    for (int i = 0; i < arguments.arg_num; i++) {
        gi_sorted.emplace_back(induced_subgraph(gi[i], vvi[i]));
    }

    std::pair<vector<VtxSet>, unsigned long long> solution = mcs(gi_sorted);

    // Convert to indices from original, unsorted graphs
    for (auto& vtx_pair : solution.first) {
        for (int i = 0; i < arguments.arg_num; i++) {
            vtx_pair.vv[i] = vvi[i][vtx_pair.vv[i]];
        }
    }

    const auto stop = steady_clock::now();
    const auto time_elapsed = duration_cast<milliseconds>(stop - start).count();

    /* Clean up the timeout thread */
    if (timeout_thread.joinable()) {
        {
            std::unique_lock<std::mutex> guard(timeout_mutex);
            abort_due_to_timeout.store(true);
            timeout_cv.notify_all();
        }
        timeout_thread.join();
    }

    const double end = clock ();

    cout << "Solution size " << solution.first.size() << std::endl;
    for (int i = 0; i < gi[0].n; i++) {
        for (auto f : solution.first) {
            if (f.vv[0] == i) {
                cout << "(" << f.vv[0];
                for (int k = 1; k < arguments.arg_num; k++) {
                    cout << " -> " << f.vv[k];
                }
                cout << ") ";
            }
        }
    }
    cout << std::endl;

    cout << "Nodes:                      " << solution.second << endl;
    cout << "CPU time (ms):              " << time_elapsed << endl;

    fprintf (stdout, "Wall-Clock Time = %f sec\n", (double)(end-begin)/CLOCKS_PER_SEC);
    
    if (aborted)
        cout << "TIMEOUT" << endl;

    if (!check_sol(gi, solution.first))
        fail("\n\n*** Error: Invalid solution\n");
        
    cout << ">>> " << solution.first.size() << " - " << solution.second << " - " << (double) time_elapsed/1000 << endl;
}

