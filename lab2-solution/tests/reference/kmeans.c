/*
 * RECONSTRUCTION FOR TESTING ONLY - not the course file.
 *
 * Sequential k-means written to behave like the kmeans.c handed out on
 * Canvas, as far as the lab handout shows (fixed input kmeans-data.txt,
 * k = 9, output kmeans-results.txt, the four progress messages). Replace it
 * with the real Canvas file when you compare: run_tests.sh accepts
 * SEQ_DIR=/path/to/canvas/files.
 */
#include <float.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_POINTS 4096
#define MAX_CLUSTERS 32

typedef struct point {
    float x;
    float y;
    int cluster;
} point;

int N;
int k;
point data[MAX_POINTS];
point cluster[MAX_CLUSTERS];

void read_data(void)
{
    N = 1797;
    k = 9;
    FILE *fp = fopen("kmeans-data.txt", "r");
    if (fp == NULL) {
        perror("Cannot open the file");
        exit(EXIT_FAILURE);
    }
    for (int i = 0; i < N; i++) {
        if (fscanf(fp, "%f %f", &data[i].x, &data[i].y) != 2) {
            fprintf(stderr, "Bad data\n");
            exit(EXIT_FAILURE);
        }
        data[i].cluster = -1;
    }
    printf("Read the problem data!\n");
    srand(0);
    for (int i = 0; i < k; i++) {
        int r = rand() % N;
        cluster[i].x = data[r].x;
        cluster[i].y = data[r].y;
    }
    fclose(fp);
}

int get_closest_centroid(int i, int k)
{
    int nearest_cluster = -1;
    double xdist, ydist, dist, min_dist;
    min_dist = dist = INT_MAX;
    for (int c = 0; c < k; c++) {
        xdist = data[i].x - cluster[c].x;
        ydist = data[i].y - cluster[c].y;
        dist = xdist * xdist + ydist * ydist;
        if (dist <= min_dist) {
            min_dist = dist;
            nearest_cluster = c;
        }
    }
    return nearest_cluster;
}

bool assign_clusters_to_points(void)
{
    bool something_changed = false;
    for (int i = 0; i < N; i++) {
        int old_cluster = data[i].cluster;
        int new_cluster = get_closest_centroid(i, k);
        data[i].cluster = new_cluster;
        if (old_cluster != new_cluster)
            something_changed = true;
    }
    return something_changed;
}

void update_cluster_centers(void)
{
    int count[MAX_CLUSTERS] = { 0 };
    point temp[MAX_CLUSTERS] = { { 0.0f, 0.0f, 0 } };

    for (int i = 0; i < N; i++) {
        int c = data[i].cluster;
        count[c]++;
        temp[c].x += data[i].x;
        temp[c].y += data[i].y;
    }
    for (int i = 0; i < k; i++) {
        cluster[i].x = temp[i].x / count[i];
        cluster[i].y = temp[i].y / count[i];
    }
}

void kmeans(void)
{
    bool somechange;
    int iter = 0;
    do {
        iter++;
        somechange = assign_clusters_to_points();
        update_cluster_centers();
    } while (somechange);
    printf("Number of iterations taken = %d\n", iter);
    printf("Computed cluster numbers successfully!\n");
}

void write_results(void)
{
    FILE *fp = fopen("kmeans-results.txt", "w");
    if (fp == NULL) {
        perror("Cannot open the file");
        exit(EXIT_FAILURE);
    }
    for (int i = 0; i < N; i++)
        fprintf(fp, "%.2f %.2f %d\n", data[i].x, data[i].y, data[i].cluster);
    fclose(fp);
    printf("Wrote the results to a file!\n");
}

int main(void)
{
    read_data();
    kmeans();
    write_results();
    return 0;
}
