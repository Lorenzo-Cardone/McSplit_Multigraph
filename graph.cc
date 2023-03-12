#include "graph.hh"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <fstream>
#include <string>

using namespace std;
constexpr int BITS_PER_UNSIGNED_INT (CHAR_BIT * sizeof(unsigned int));

static void fail(std::string msg) {
    std::cerr << msg << std::endl;
    exit(1);
}

Graph::Graph(unsigned int n) {
    this->n = n;
    label.resize(n, 0u);
    adjmat.resize(n, std::vector<unsigned int>(n, false));
}
Graph::Graph(unsigned int n, const std::string graph_name) {
    this->n = n;
    label.resize(n, 0u);
    adjmat.resize(n, std::vector<unsigned int>(n, false));
    this->name = graph_name;
}

Graph induced_subgraph(struct Graph& g, std::vector<int> vv) {
    //cout << "g: " << g.name << endl;
    Graph subg(vv.size(), g.name);
    for (int i=0; i<subg.n; i++)
        for (int j=0; j<subg.n; j++)
            subg.adjmat[i][j] = g.adjmat[vv[i]][vv[j]];

    for (int i=0; i<subg.n; i++)
        subg.label[i] = g.label[vv[i]];
    return subg;
}

void add_edge(Graph& g, int v, int w, bool directed=false, unsigned int val=1) {
    if (v != w) {
        if (directed) {
            g.adjmat[v][w] |= val;
            g.adjmat[w][v] |= (val<<16);
        } else {
            g.adjmat[v][w] = val;
            g.adjmat[w][v] = val;
        }
    } else {
        // To indicate that a vertex has a loop, we set the most
        // significant bit of its label to 1
        g.label[v] |= (1u << (BITS_PER_UNSIGNED_INT-1));
    }
}

struct Graph readDimacsGraph(std::string filename, bool directed, bool vertex_labelled) {
    struct Graph g(0, filename);


    ifstream f(filename);

    if (!f)
        fail("Cannot open file");

    char* line = NULL;
    size_t nchar = 0;

    int nvertices = 0;
    int medges = 0;
    int v, w;
    int edges_read = 0;
    int label;

    for (std::string line; getline(f, line, '\n'); ) {
        if (line.size() > 0) {
            switch (line[0]) {
            case 'p':
                if (sscanf(line.c_str(), "p edge %d %d", &nvertices, &medges)!=2)
                    fail("Error reading a line beginning with p.\n");
                g = Graph(nvertices, filename);
                break;
            case 'e':
                if (sscanf(line.c_str(), "e %d %d", &v, &w)!=2)
                    fail("Error reading a line beginning with e.\n");
                add_edge(g, v-1, w-1, directed);
                edges_read++;
                break;
            case 'n':
                if (sscanf(line.c_str(), "n %d %d", &v, &label)!=2)
                    fail("Error reading a line beginning with n.\n");
                if (vertex_labelled)
                    g.label[v-1] |= label;
                break;
            }
        }
    }

    if (medges>0 && edges_read != medges) fail("Unexpected number of edges.");

    f.close();
    return g;
}

struct Graph readLadGraph(std::string filename, bool directed) {
    //cout << "g: " << filename << endl;
    struct Graph g(0, filename);
    //cout << "g: " << g.name << endl;
    FILE* f;
    
    if ((f=fopen(filename.c_str(), "r"))==NULL)
        fail("Cannot open file");

    int nvertices = 0;
    int w;

    if (fscanf(f, "%d", &nvertices) != 1)
        fail("Number of vertices not read correctly.\n");
    g = Graph(nvertices, filename);

    for (int i=0; i<nvertices; i++) {
        int edge_count;
        if (fscanf(f, "%d", &edge_count) != 1)
            fail("Number of edges not read correctly.\n");
        for (int j=0; j<edge_count; j++) {
            if (fscanf(f, "%d", &w) != 1)
                fail("An edge was not read correctly.\n");
            add_edge(g, i, w, directed);
        }
    }

    fclose(f);
    return g;
}

int read_word(FILE *fp) {
    unsigned char a[2];
    if (fread(a, 1, 2, fp) != 2)
        fail("Error reading file.\n");
    return (int)a[0] | (((int)a[1]) << 8);
}

struct Graph readBinaryGraph(std::string filename, bool directed, bool edge_labelled,
        bool vertex_labelled)
{
    struct Graph g(0);
    FILE* f;
    
    if ((f=fopen(filename.c_str(), "rb"))==NULL)
        fail("Cannot open file");

    int nvertices = read_word(f);
    g = Graph(nvertices, filename);

    // Labelling scheme: see
    // https://github.com/ciaranm/cp2016-max-common-connected-subgraph-paper/blob/master/code/solve_max_common_subgraph.cc
    int m = g.n * 33 / 100;
    int p = 1;
    int k1 = 0;
    int k2 = 0;
    while (p < m && k1 < 16) {
        p *= 2;
        k1 = k2;
        k2++;
    }
    
    for (int i=0; i<nvertices; i++) {
        int label = (read_word(f) >> (16-k1));
        if (vertex_labelled)
            g.label[i] |= label;
    }

    for (int i=0; i<nvertices; i++) {
        int len = read_word(f);
        for (int j=0; j<len; j++) {
            int target = read_word(f);
            int label = (read_word(f) >> (16-k1)) + 1;
            add_edge(g, i, target, directed, edge_labelled ? label : 1);
        }
    }
    fclose(f);
    return g;
}

int custom_read_word(FILE* fp) {
  unsigned char a[2];
  if (fread(a, 1, 2, fp) != 2) fail("Error reading file.\n");
  return static_cast<int>(a[0]) | ((static_cast<int>(a[1])) << 8);
}

struct Graph read_bin_graph(const std::string filename, bool directed, bool edge_labelled,
                     bool vertex_labelled) {
  Graph g(0);
  FILE* f;

  if ((f = fopen(filename.c_str(), "rb")) == NULL) fail("Cannot open file");

  int nvertices = custom_read_word(f);
  g = Graph(nvertices, filename);

  // Labelling scheme: see
  // https://github.com/ciaranm/cp2016-max-common-connected-subgraph-paper/blob/master/code/solve_max_common_subgraph.cc
  int m = g.n * 33 / 100;
  int p = 1;
  int k1 = 0;
  int k2 = 0;
  while (p < m && k1 < 16) {
    p *= 2;
    k1 = k2;
    k2++;
  }

  for (int i = 0; i < nvertices; i++) {
    int label = (custom_read_word(f) >> (16 - k1));
    if (vertex_labelled) g.label[i] |= label;
  }

  for (int i = 0; i < nvertices; i++) {
    int len = custom_read_word(f);
    for (int j = 0; j < len; j++) {
      int target = custom_read_word(f);
      int label = (custom_read_word(f) >> (16 - k1)) + 1;
      add_edge(g, i, target, directed, edge_labelled ? label : 1);
    }
  }
  fclose(f);
  return g;
}

Graph read_ioi_graph(const std::string filename, bool directed,
                     bool vertex_labelled) {
  FILE* f;

  if ((f = fopen(filename.c_str(), "r")) == NULL) fail("Cannot open file");

  int n, m;

  fscanf(f, "%d %d", &n, &m);

  Graph g(n, filename);

  for (int i = 0; i < n; i++) {
    int label;
    fscanf(f, "%d", &label);
    if (vertex_labelled) g.label[i] |= label;
  }

  for (int i = 0; i < m; i++) {
    int v, w;
    fscanf(f, "%d %d", &v, &w);
    add_edge(g, v, w, directed, 1);
  }

  fclose(f);
  return g;
}

struct Graph readGraph(std::string filename, char format, bool directed, bool edge_labelled, bool vertex_labelled) {
    struct Graph g(0, filename);
    if (format=='D') g = readDimacsGraph(filename, directed, vertex_labelled);
    else if (format=='L') g = readLadGraph(filename, directed);
    else if (format=='B') g = readBinaryGraph(filename, directed, edge_labelled, vertex_labelled);
    else if (format=='E') g = read_bin_graph(filename, directed, edge_labelled, vertex_labelled);
    else if (format=='I') g = read_ioi_graph(filename, directed, vertex_labelled);
    else fail("Unknown graph format\n");
    //cout << "g: " << g.name;
    return g;
}
