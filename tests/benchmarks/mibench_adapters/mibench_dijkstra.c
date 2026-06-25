/*
 * MiBench Dijkstra Adapter for RP2350 Benchmark Suite
 *
 * Uses a deterministic synthetic graph to avoid file I/O on embedded targets.
 */

#include "../benchmarks.h"

#define MIBENCH_DIJKSTRA_NUM_NODES 100
#define MIBENCH_DIJKSTRA_NONE 9999
#define MIBENCH_DIJKSTRA_QUEUE_CAPACITY 10000

typedef struct
{
  int dist;
  int prev;
} dijkstra_node_t;

typedef struct
{
  int node;
  int dist;
  int prev;
} dijkstra_queue_item_t;

static int dijkstra_adj_matrix[MIBENCH_DIJKSTRA_NUM_NODES][MIBENCH_DIJKSTRA_NUM_NODES];
static dijkstra_node_t dijkstra_nodes[MIBENCH_DIJKSTRA_NUM_NODES];
static dijkstra_queue_item_t dijkstra_queue[MIBENCH_DIJKSTRA_QUEUE_CAPACITY];
static int dijkstra_graph_initialized = 0;

static int dijkstra_path_checksum(int end_node)
{
  int checksum = dijkstra_nodes[end_node].dist;
  int node = end_node;

  while (node != MIBENCH_DIJKSTRA_NONE)
  {
    checksum += node;
    node = dijkstra_nodes[node].prev;
  }

  return checksum;
}

static void init_dijkstra_graph(void)
{
  if (dijkstra_graph_initialized)
  {
    return;
  }

  for (int row = 0; row < MIBENCH_DIJKSTRA_NUM_NODES; row++)
  {
    for (int col = 0; col < MIBENCH_DIJKSTRA_NUM_NODES; col++)
    {
      if (row == col)
      {
        dijkstra_adj_matrix[row][col] = 0;
      }
      else if (col == row + 1 || (row > 0 && col == row - 1))
      {
        dijkstra_adj_matrix[row][col] = 1 + ((row + col) % 7);
      }
      else if (((row * 17 + col * 13) % 11) < 3)
      {
        dijkstra_adj_matrix[row][col] = 2 + ((row * 5 + col * 3) % 29);
      }
      else
      {
        dijkstra_adj_matrix[row][col] = MIBENCH_DIJKSTRA_NONE;
      }
    }
  }

  dijkstra_graph_initialized = 1;
}

static int run_dijkstra_path(int start_node, int end_node)
{
  int queue_head = 0;
  int queue_tail = 0;

  for (int index = 0; index < MIBENCH_DIJKSTRA_NUM_NODES; index++)
  {
    dijkstra_nodes[index].dist = MIBENCH_DIJKSTRA_NONE;
    dijkstra_nodes[index].prev = MIBENCH_DIJKSTRA_NONE;
  }

  dijkstra_nodes[start_node].dist = 0;
  dijkstra_queue[queue_tail].node = start_node;
  dijkstra_queue[queue_tail].dist = 0;
  dijkstra_queue[queue_tail].prev = MIBENCH_DIJKSTRA_NONE;
  queue_tail++;

  while (queue_head < queue_tail)
  {
    dijkstra_queue_item_t current = dijkstra_queue[queue_head++];

    for (int node = 0; node < MIBENCH_DIJKSTRA_NUM_NODES; node++)
    {
      int edge_cost = dijkstra_adj_matrix[current.node][node];

      if (edge_cost == MIBENCH_DIJKSTRA_NONE)
      {
        continue;
      }

      if (dijkstra_nodes[node].dist == MIBENCH_DIJKSTRA_NONE || dijkstra_nodes[node].dist > current.dist + edge_cost)
      {
        dijkstra_nodes[node].dist = current.dist + edge_cost;
        dijkstra_nodes[node].prev = current.node;

        if (queue_tail < MIBENCH_DIJKSTRA_QUEUE_CAPACITY)
        {
          dijkstra_queue[queue_tail].node = node;
          dijkstra_queue[queue_tail].dist = dijkstra_nodes[node].dist;
          dijkstra_queue[queue_tail].prev = current.node;
          queue_tail++;
        }
      }
    }
  }

  return dijkstra_path_checksum(end_node);
}

int bench_mibench_dijkstra(int iterations)
{
  int checksum = 0;

  init_dijkstra_graph();

  for (int iteration = 0; iteration < iterations; iteration++)
  {
    int start_node = (iteration * 7) % MIBENCH_DIJKSTRA_NUM_NODES;
    int end_node = (start_node + 33 + iteration) % MIBENCH_DIJKSTRA_NUM_NODES;

    checksum = run_dijkstra_path(start_node, end_node);
  }

  return checksum;
}

void init_mibench_dijkstra(void)
{
  register_benchmark_ex("mibench_dijkstra", bench_mibench_dijkstra, 64, "MiBench: Dijkstra shortest path", 199);
}
