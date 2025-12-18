# Assumptions:
# - matrix is a non-empty square matrix (n x n)
# - all rows have equal length n
# - rotation is performed in place
# - rotates the matrix 90 degrees clockwise
def rotate_in_place(matrix):
    n = len(matrix)
    last = n - 1

    # process the matrix layer by layer (from outer to inner)
    for i in range(n // 2):
        top = i
        bottom = last - i
        for j in range(i, bottom):
            left = j
            right = last - j

            temp                   = matrix[top   ][left  ]
            matrix[top   ][left  ] = matrix[right ][top   ]
            matrix[right ][top   ] = matrix[bottom][right ]
            matrix[bottom][right ] = matrix[left  ][bottom]
            matrix[left  ][bottom] = temp

MATRIX1 = [
    [ 1,  2,  3,  4],
    [ 5,  6,  7,  8],
    [ 9, 10, 11, 12],
    [13, 14, 15, 16]
]

EXPECTED1 = [
    [13,  9,  5,  1],
    [14, 10,  6,  2],
    [15, 11,  7,  3],
    [16, 12,  8,  4]
]

MATRIX2 = [
    [ 1,  2,  3,  4,  5,  6,  7],
    [ 8,  9, 10, 11, 12, 13, 14],
    [15, 16, 17, 18, 19, 20, 21],
    [22, 23, 24, 25, 26, 27, 28],
    [29, 30, 31, 32, 33, 34, 35],
    [36, 37, 38, 39, 40, 41, 42],
    [43, 44, 45, 46, 47, 48, 49]
]

EXPECTED2 = [
    [43, 36, 29, 22, 15,  8,  1],
    [44, 37, 30, 23, 16,  9,  2],
    [45, 38, 31, 24, 17, 10,  3],
    [46, 39, 32, 25, 18, 11,  4],
    [47, 40, 33, 26, 19, 12,  5],
    [48, 41, 34, 27, 20, 13,  6],
    [49, 42, 35, 28, 21, 14,  7]
]

for testcase, expected in ((MATRIX1, EXPECTED1), (MATRIX2, EXPECTED2)):
    rotate_in_place(testcase)
    if testcase != expected:
        print('Testcase failed. Actual vs. Expected:')
        n = len(expected)
        for r1, r2 in zip(testcase, expected):
            print(f'{str(r1):<{4*n}} {r2}')
        print()
    else:
        print('Testcase OK!')
