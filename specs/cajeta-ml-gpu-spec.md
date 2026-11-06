# cajeta-ml-gpu — cajeta-ml as a GPU library

Status: draft, being written with Julian (2026-11-06).

## 1. Definition

### 1.1 Purpose
cajeta-ml becomes a GPU library: kernels and the support to run them, general
purpose, with cajeta-llm and cajeta-xgboost as its first two consumers.

### 1.2 Sequence (Julian 2026-11-06)
1. cajeta-ml gains the GPU kernels and kernel support, general purpose, shaped by
   what cajeta-llm and cajeta-xgboost need.
2. When that is done, cajeta-llm incorporates cajeta-ml's components where
   appropriate, in place of its own copies.
