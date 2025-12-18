# quantum-systems-test
Quantum Systems remote case study

## 1. cpp task
TODO

## 2. python task. Matrix Rotation Fix

The original implementation attempted to rotate the matrix by directly reassigning values while iterating over all rows and columns. This approach failed because elements were overwritten before their original values were used elsewhere, leading to incorrect results.

The fix uses a layer by layer rotation strategy. A square matrix can be viewed as a set of concentric layers, starting from the outer border and moving inward. Each layer is rotated independently.

For a given layer, four corresponding elements are rotated at a time: top, right, bottom, and left. A temporary variable stores one value so that all four positions can be updated without losing data. After completing one layer, the algorithm proceeds to the next inner layer.

This method rotates the matrix 90 degrees clockwise, works entirely in place, preserves all values during rotation, and runs in $O(n^2)$ time with constant extra space.

## 3. cpp class design and implementation
TODO