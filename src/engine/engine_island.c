// Copyright 2023 DeepMind Technologies Limited
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "engine/engine_island.h"

#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include <mujoco/mjdata.h>
#include <mujoco/mjmacro.h>
#include <mujoco/mjmodel.h>
#include <mujoco/mjxmacro.h>
#include "engine/engine_core_constraint.h"
#include "engine/engine_crossplatform.h"
#include "engine/engine_io.h"
#include "engine/engine_support.h"
#include "engine/engine_util_errmem.h"

#ifdef MEMORY_SANITIZER
  #include <sanitizer/msan_interface.h>
#endif



// find disjoint subgraphs ("islands") given sparse symmetric adjacency matrix
//   arguments:
//     island  (nr)   - island index assigned to vertex, -1 if vertex has no edges
//     nr             - number of rows/columns of adjacency matrix
//     rownnz  (nr)   - matrix row nonzeros
//     rowadr  (nr)   - matrix row addresses
//     colind  (nnz)  - matrix column indices
//     stack   (nnz)  - stack space
//   returns number of islands
//   note: column indices are not required to be unique or sorted
int mj_floodFill(int* island, int nr, const int* rownnz, const int* rowadr, const int* colind,
                 int* stack) {
  // initialize island count, set ids to -1
  int nisland = 0;
  for (int i=0; i < nr; i++) island[i] = -1;

  // iterate over vertices, discover islands
  for (int i=0; i < nr; i++) {
    // vertex already in island or singleton with no edges: skip
    if (island[i] != -1 || !rownnz[i]) {
      continue;
    }

    // push i onto stack
    int nstack = 0;
    stack[nstack++] = i;

    // DFS traversal of island
    while (nstack) {
      // pop v from stack
      int v = stack[--nstack];

      // if v is already assigned, continue
      if (island[v] != -1) {
        continue;
      }

      // assign v to current island
      island[v] = nisland;

      // push adjacent vertices onto stack
      memcpy(stack + nstack, colind + rowadr[v], rownnz[v]*sizeof(int));
      nstack += rownnz[v];
    }

    // island is filled: increment nisland
    nisland++;
  }

  return nisland;
}



// clear island-related arena pointers in mjData
static void clearIsland(mjData* d, size_t parena) {
#define X(type, name, nr, nc) d->name = NULL;
  MJDATA_ARENA_POINTERS_ISLAND
#undef X
  d->nefc = 0;
  d->nisland = 0;
  d->parena = parena;

  // poison remaining memory
#ifdef ADDRESS_SANITIZER
  ASAN_POISON_MEMORY_REGION(
    (char*)d->arena + d->parena, (d->nstack - d->pstack) * sizeof(mjtNum) - d->parena);
#endif
}



// return upper bound on number of tree-tree edges
static int countMaxEdge(const mjModel* m, const mjData* d) {
  int nedge_max = 0;
  nedge_max += 2*d->ncon;  // contact: 2 edges
  nedge_max += 2*d->ne;    // equality: 2 edges
  nedge_max += d->nf;      // joint friction: 1 edge (always within same tree)

  // tendon limits and friction add up to tendon_num edges
  for (int i=0; i < m->ntendon; i++) {
    if (m->tendon_frictionloss[i]) {
      nedge_max += m->tendon_num[i];
    }
    if (m->tendon_limited[i]) {
      nedge_max += m->tendon_num[i];
    }
  }

  return nedge_max;
}



// return id of next tree in Jacobian row i that is different from tree, -1 if not found
//   write the index of the found tree to *index if given
//   start search from *index if given, otherwise 0
//   if J is (dense/sparse) *index is the (column/nonzro) index, respectively
static int treeNext(const mjModel* m, const mjData* d, int tree, int i, int *index) {
  int tree_next = -1;
  int j0 = index ? *index : 0;  // start searching at *index if given, otherwise 0
  int j;  // loop variable, saved to *index

  // sparse
  if (mj_isSparse(m)) {
    int rownnz = d->efc_J_rownnz[i];
    int* colind = d->efc_J_colind + d->efc_J_rowadr[i];

    // loop over remaining nonzeros, look for different tree
    for (j=j0; j < rownnz; j++) {
      int tree_j = m->dof_treeid[colind[j]];
      if (tree_j != tree) {
        // found different tree
        tree_next = tree_j;
        break;
      }
    }
  }

  // dense
  else {
    int nv = m->nv;

    // scan row, look for different tree
    for (j=j0; j < nv; j++) {
      if (d->efc_J[nv*i + j]) {
        int tree_j = m->dof_treeid[j];
        if (tree_j != tree) {
          // found different tree
          tree_next = tree_j;
          break;
        }
      }
    }
  }

  // save last index
  if (index) *index = j;

  return tree_next;
}



// add 0 edges, 1 self-edge or 2 flipped edges to array, increment treenedge
//   return current number of edges
static int addEdge(int* treenedge, int* edge, int nedge, int tree1, int tree2, int nedge_max) {
  // handle the static tree
  if (tree1 == -1 && tree2 == -1) {
    mjERROR("self-edge of the static tree");  // SHOULD NOT OCCUR
    return 0;
  }
  if (tree1 == -1) tree1 = tree2;
  if (tree2 == -1) tree2 = tree1;

  // previous edge
  int p1 = nedge ? edge[2*nedge - 2] : -1;
  int p2 = nedge ? edge[2*nedge - 1] : -1;

  // === self edge
  if (tree1 == tree2) {
    // same as previous edge, return
    if (nedge && tree1 == p1 && tree1 == p2) {
      return nedge;
    }

    // check size
    if (nedge >= nedge_max) {
      mjERROR("edge array too small");
      return 0;
    }

    // add tree1-tree1 self-edge
    edge[2*nedge + 0] = tree1;
    edge[2*nedge + 1] = tree1;
    treenedge[tree1]++;
    return nedge + 1;
  }

  // === non-self edge
  if (nedge && ((tree1 == p1 && tree2 == p2) || (tree1 == p2 && tree2 == p1))) {
    // same as previous edge, return
    return nedge;
  }

  // check size
  if (nedge + 2 > nedge_max) {
    mjERROR("edge array too small");
    return 0;
  }

  // add tree1-tree2 and tree2-tree1
  edge[2*nedge + 0] = tree1;
  edge[2*nedge + 1] = tree2;
  edge[2*nedge + 2] = tree2;
  edge[2*nedge + 3] = tree1;
  treenedge[tree1]++;
  treenedge[tree2]++;
  return nedge + 2;
}



// find tree-tree edges, increment treenedge counters, return total number of edges
static int findEdges(const mjModel* m, const mjData* d, int* treenedge, int* edge, int nedge_max) {
  int nefc = d->nefc;
  int efc_type = -1;
  int efc_id = -1;
  int tree1, tree2;

  // clear treenedge
  memset(treenedge, 0, m->ntree*sizeof(int));

  int nedge = 0;
  for (int i=0; i < nefc; i++) {
    // row i is still in the same constraint: skip
    if (efc_type == d->efc_type[i] && efc_id == d->efc_id[i]) {
      continue;
    }
    efc_type = d->efc_type[i];
    efc_id = d->efc_id[i];

    // ==== fast handling of special cases

    // joint friction
    if (efc_type == mjCNSTR_FRICTION_DOF) {
      tree1 = m->dof_treeid[efc_id];
      nedge = addEdge(treenedge, edge, nedge, tree1, tree1, nedge_max);
      continue;
    }

    // joint limit
    if (efc_type == mjCNSTR_LIMIT_JOINT) {
      tree1 = m->dof_treeid[m->jnt_dofadr[efc_id]];
      nedge = addEdge(treenedge, edge, nedge, tree1, tree1, nedge_max);
      continue;
    }

    // contact
    if (efc_type == mjCNSTR_CONTACT_FRICTIONLESS ||
        efc_type == mjCNSTR_CONTACT_PYRAMIDAL ||
        efc_type == mjCNSTR_CONTACT_ELLIPTIC) {
      tree1 = m->body_treeid[m->geom_bodyid[d->contact[efc_id].geom1]];
      tree2 = m->body_treeid[m->geom_bodyid[d->contact[efc_id].geom2]];
      nedge = addEdge(treenedge, edge, nedge, tree1, tree2, nedge_max);
      continue;
    }

    // connect or weld constraints
    if (efc_type == mjCNSTR_EQUALITY) {
      mjtEq eq_type = m->eq_type[efc_id];
      if (eq_type == mjEQ_CONNECT || eq_type == mjEQ_WELD) {
        tree1 = m->body_treeid[m->eq_obj1id[efc_id]];
        tree2 = m->body_treeid[m->eq_obj2id[efc_id]];
        nedge = addEdge(treenedge, edge, nedge, tree1, tree2, nedge_max);
        continue;
      }
    }

    // ==== generic case: scan Jacobian
    int index = 0;
    tree1 = treeNext(m, d, -1, i, &index);
    tree2 = treeNext(m, d, tree1, i, &index);

    if (tree2 == -1) {
      // 1 tree found: add self-edge
      nedge = addEdge(treenedge, edge, nedge, tree1, tree1, nedge_max);
    } else {
      // 2 trees found: add edge, keep scanning and adding until no more trees
      nedge = addEdge(treenedge, edge, nedge, tree1, tree2, nedge_max);
      int tree3 = treeNext(m, d, tree2, i, &index);
      while (tree3 > -1 && tree3 != tree2) {
        tree1 = tree2;
        tree2 = tree3;
        nedge = addEdge(treenedge, edge, nedge, tree1, tree2, nedge_max);
        tree3 = treeNext(m, d, tree2, i, &index);
      }
    }
  }

  return nedge;
}



// discover islands:
//   nisland, island_dofadr, dof_island, dof_islandnext, island_efcadr, efc_island, efc_islandnext
void mj_island(const mjModel* m, mjData* d) {
  int nv = m->nv, nefc = d->nefc, ntree=m->ntree;

  // no constraints: quick return
  if (!nefc) {
    d->nisland = 0;
    return;
  }

  mjMARKSTACK;

  // allocate edge array
  int nedge_max = countMaxEdge(m, d);
  int* edge = mj_stackAllocInt(d, 2*nedge_max);

  // get tree-tree edges and rownnz counts from efc arrays
  int* rownnz = mj_stackAllocInt(d, ntree);  // number of edges per tree
  int nedge = findEdges(m, d, rownnz, edge, nedge_max);

  // compute starting address of tree's column indices while resetting rownnz
  int* rowadr = mj_stackAllocInt(d, ntree);
  rowadr[0] = 0;
  for (int r=1; r < ntree; r++) {
    rowadr[r] = rowadr[r-1] + rownnz[r-1];
    rownnz[r-1] = 0;
  }
  rownnz[ntree-1] = 0;

  // copy column indices: list each tree's neighbors
  int* colind = mj_stackAllocInt(d, nedge);
  for (int e=0; e < nedge; e++) {
    int row = edge[2*e];
    int col = edge[2*e + 1];
    colind[rowadr[row] + rownnz[row]++] = col;
  }

  // discover islands
  int* tree_island = mj_stackAllocInt(d, ntree);  // id of island assigned to tree
  int* stack = mj_stackAllocInt(d, nedge);
  d->nisland = mj_floodFill(tree_island, ntree, rownnz, rowadr, colind, stack);

  // ========== begin arena allocation of MJDATA_ARENA_POINTERS_ISLAND
#undef MJ_M
#define MJ_M(n) m->n
#undef MJ_D
#define MJ_D(n) d->n

  size_t parena_old = d->parena;

#define X(type, name, nr, nc)                                             \
  d->name = mj_arenaAlloc(d, sizeof(type) * (nr) * (nc), _Alignof(type)); \
  if (!d->name) {                                                         \
    mj_warning(d, mjWARN_CNSTRFULL, d->nstack * sizeof(mjtNum));          \
    clearIsland(d, parena_old);                                           \
    mjFREESTACK;                                                          \
    return;                                                               \
  }

  MJDATA_ARENA_POINTERS_ISLAND

#undef X

#undef MJ_M
#define MJ_M(n) n
#undef MJ_D
#define MJ_D(n) n
  // ========== end arena allocation

  // prepare island_last: id of last element in each island
  int* island_last = mj_stackAllocInt(d, d->nisland);
  for (int i=0; i < d->nisland; i++) {
    island_last[i] = -1;
  }

  // compute island_dofadr, dof_island, dof_islandnext
  int nisland_found = 0;
  for (int i=0; i < nv; i++) {
    // dof_island
    int island = tree_island[m->dof_treeid[i]];;
    d->dof_island[i] = island;

    // island_dofadr, dof_islandnext
    if (island == -1) {
      // dof is not in any island (unconstrained)
      d->dof_islandnext[i] = -1;
      continue;
    } else {
      int last = island_last[island];
      if (last == -1) {
        // first dof: set island_dofadr, increment nisland_found
        d->island_dofadr[island] = i;
        nisland_found++;
      } else {
        // subsequent dof: point last dof to i
        d->dof_islandnext[last] = i;
      }
      island_last[island] = i;
    }
  }

  // sanity check, SHOULD NOT OCCUR
  if (nisland_found != d->nisland) {
    mjERROR("not all islands assigned to dofs");
  }

  // finalize dof_islandnext: mark last dof in each island with -1
  for (int i=0; i < d->nisland; i++) {
    d->dof_islandnext[island_last[i]] = -1;
  }

  // reset island_last
  for (int i=0; i < d->nisland; i++) {
    island_last[i] = -1;
  }

  // compute island_efcadr, efc_island, efc_islandnext
  nisland_found = 0;
  for (int i=0; i < nefc; i++) {
    // efc_island
    int island = tree_island[treeNext(m, d, -1, i, NULL)];
    d->efc_island[i] = island;

    // island_efcadr, efc_islandnext
    if (island == -1) {
      mjERROR("constraint %d not in any island", i);  // SHOULD NOT OCCUR
    } else {
      int last = island_last[island];
      if (last == -1) {
        // first constraint: set island_efcadr, increment nisland_found
        d->island_efcadr[island] = i;
        nisland_found++;
      } else {
        // subsequent constraint: point last constraint to i
        d->efc_islandnext[last] = i;
      }
      island_last[island] = i;
    }
  }

  // sanity check, SHOULD NOT OCCUR
  if (nisland_found != d->nisland) {
    mjERROR("not all islands assigned to constraints");
  }

  // finalize efc_islandnext: mark last constraint in each island with -1
  for (int i=0; i < d->nisland; i++) {
    d->efc_islandnext[island_last[i]] = -1;
  }

  mjFREESTACK;
}
