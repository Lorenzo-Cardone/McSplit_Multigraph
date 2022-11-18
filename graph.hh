#include <limits.h>
#include <stdbool.h>
#include <fstream>

#include <vector>

struct Graph {
    int n;
    std::vector<std::vector<unsigned int>> adjmat;
    std::vector<unsigned int> label;
    Graph(unsigned int n);
};

Graph induced_subgraph(struct Graph& g, std::vector<int> vv);

Graph readGraph(std::string filename, char format, bool directed, bool edge_labelled, bool vertex_labelled);

