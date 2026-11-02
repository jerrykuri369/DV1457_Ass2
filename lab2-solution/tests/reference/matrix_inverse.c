/*
 * RECONSTRUCTION FOR TESTING ONLY - not the course file.
 *
 * Sequential matrix inversion written to behave like the matrix_inverse.c
 * handed out on Canvas, as far as the lab handout shows (options, defaults,
 * matrix initialisation and the " %5.2f" output, e.g. the -n 4 -I fast
 * example in the handout). Replace it with the real Canvas file when you
 * compare: run_tests.sh accepts SEQ_DIR=/path/to/canvas/files.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SIZE 4096

typedef double matrix[MAX_SIZE][MAX_SIZE];

int N;
int maxnum;
char *Init;
int PRINT;
matrix A;
matrix I = { 0.0 };

void find_inverse(void);
void Init_Matrix(void);
void Print_Matrix(matrix M, char name[]);
void Init_Default(void);
int Read_Options(int, char **);

int main(int argc, char **argv)
{
    printf("Matrix Inverse\n");
    Init_Default();
    Read_Options(argc, argv);
    Init_Matrix();
    find_inverse();
    if (PRINT == 1)
        Print_Matrix(I, "Inversed");
    return 0;
}

void find_inverse(void)
{
    int row, col, p;
    double pivalue;

    for (p = 0; p < N; p++) {
        pivalue = A[p][p];
        for (col = 0; col < N; col++) {
            A[p][col] = A[p][col] / pivalue;
            I[p][col] = I[p][col] / pivalue;
        }
        assert(A[p][p] == 1.0);

        double multiplier;
        for (row = 0; row < N; row++) {
            multiplier = A[row][p];
            if (row != p) {
                for (col = 0; col < N; col++) {
                    A[row][col] = A[row][col] - A[p][col] * multiplier;
                    I[row][col] = I[row][col] - I[p][col] * multiplier;
                }
                assert(A[row][p] == 0.0);
            }
        }
    }
}

void Init_Matrix(void)
{
    int row, col;

    for (row = 0; row < N; row++)
        for (col = 0; col < N; col++)
            if (row == col)
                I[row][col] = 1.0;

    printf("\nsize      = %dx%d ", N, N);
    printf("\nmaxnum    = %d \n", maxnum);
    printf("Init\t  = %s \n", Init);
    printf("Initializing matrix...");

    if (strcmp(Init, "rand") == 0) {
        for (row = 0; row < N; row++)
            for (col = 0; col < N; col++) {
                if (row == col)
                    A[row][col] = (double)(rand() % maxnum) + 5.0;
                else
                    A[row][col] = (double)(rand() % maxnum) + 1.0;
            }
    }
    if (strcmp(Init, "fast") == 0) {
        for (row = 0; row < N; row++)
            for (col = 0; col < N; col++) {
                if (row == col)
                    A[row][col] = 5.0;
                else
                    A[row][col] = 2.0;
            }
    }
    printf("done \n\n");
}

void Print_Matrix(matrix M, char name[])
{
    int row, col;

    printf("%s Matrix:\n", name);
    for (row = 0; row < N; row++) {
        for (col = 0; col < N; col++)
            printf(" %5.2f", M[row][col]);
        printf("\n");
    }
    printf("\n\n");
}

void Init_Default(void)
{
    N = 5;
    Init = "fast";
    maxnum = 15.0;
    PRINT = 1;
}

int Read_Options(int argc, char **argv)
{
    char *prog = *argv;

    while (++argv, --argc > 0)
        if (**argv == '-')
            switch (*++*argv) {
            case 'n':
                --argc;
                N = atoi(*++argv);
                break;
            case 'h':
                printf("\nHELP: try matinv -u \n\n");
                exit(0);
            case 'u':
                printf("\nUsage: matinv [-n problemsize]\n");
                printf("           [-D] show default values \n");
                printf("           [-h] help \n");
                printf("           [-I init_type] fast/rand \n");
                printf("           [-m maxnum] max random no \n");
                printf("           [-P print_switch] 0/1 \n");
                exit(0);
            case 'D':
                printf("\nDefault:  n         = %d ", N);
                printf("\n          Init      = rand");
                printf("\n          maxnum    = 5 ");
                printf("\n          P         = 0 \n\n");
                exit(0);
            case 'I':
                --argc;
                Init = *++argv;
                break;
            case 'm':
                --argc;
                maxnum = atoi(*++argv);
                break;
            case 'P':
                --argc;
                PRINT = atoi(*++argv);
                break;
            default:
                printf("%s: ignored option: -%s\n", prog, *argv);
                printf("HELP: try %s -u \n\n", prog);
                break;
            }
    return 0;
}
