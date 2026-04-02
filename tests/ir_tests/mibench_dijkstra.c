/* MiBench Dijkstra - regression detection for -O2
 * Shortest path on a deterministic synthetic 100-node graph.
 */
#include <stdio.h>

#define NUM_NODES 100
#define NONE 9999
#define QUEUE_CAPACITY 10000

typedef struct {
  int dist;
  int prev;
} dijkstra_node_t;

typedef struct {
  int node;
  int dist;
  int prev;
} dijkstra_queue_item_t;

static int adj_matrix[NUM_NODES][NUM_NODES];
static dijkstra_node_t nodes[NUM_NODES];
static dijkstra_queue_item_t queue[QUEUE_CAPACITY];

static void init_graph(void)
{
  for (int row = 0; row < NUM_NODES; row++) {
    for (int col = 0; col < NUM_NODES; col++) {
      if (row == col) {
        adj_matrix[row][col] = 0;
      } else if (col == row + 1 || (row > 0 && col == row - 1)) {
        adj_matrix[row][col] = 1 + ((row + col) % 7);
      } else if (((row * 17 + col * 13) % 11) < 3) {
        adj_matrix[row][col] = 2 + ((row * 5 + col * 3) % 29);
      } else {
        adj_matrix[row][col] = NONE;
      }
    }
  }
}

static int path_checksum(int end_node)
{
  int checksum = nodes[end_node].dist;
  int node = end_node;

  while (node != NONE) {
    checksum += node;
    node = nodes[node].prev;
  }

  return checksum;
}

static int run_dijkstra(int start_node, int end_node)
{
  int queue_head = 0;
  int queue_tail = 0;

  for (int index = 0; index < NUM_NODES; index++) {
    nodes[index].dist = NONE;
    nodes[index].prev = NONE;
  }

  nodes[start_node].dist = 0;
  queue[queue_tail].node = start_node;
  queue[queue_tail].dist = 0;
  queue[queue_tail].prev = NONE;
  queue_tail++;

  while (queue_head < queue_tail) {
    dijkstra_queue_item_t current = queue[queue_head++];

    for (int node = 0; node < NUM_NODES; node++) {
      int edge_cost = adj_matrix[current.node][node];

      if (edge_cost == NONE)
        continue;

      if (nodes[node].dist == NONE || nodes[node].dist > current.dist + edge_cost) {
        nodes[node].dist = current.dist + edge_cost;
        nodes[node].prev = current.node;

        if (queue_tail < QUEUE_CAPACITY) {
          queue[queue_tail].node = node;
          queue[queue_tail].dist = nodes[node].dist;
          queue[queue_tail].prev = current.node;
          queue_tail++;
        }
      }
    }
  }

  return path_checksum(end_node);
}

int bench_mibench_dijkstra(void)
{
  int checksum = 0;
  int iterations = 64;

  init_graph();

  for (int iteration = 0; iteration < iterations; iteration++) {
    int start_node = (iteration * 7) % NUM_NODES;
    int end_node = (start_node + 33 + iteration) % NUM_NODES;
    checksum = run_dijkstra(start_node, end_node);
  }

  return checksum;
}

int main(void)
{
    printf("dijkstra: %d\n", bench_mibench_dijkstra());
    return 0;
}
