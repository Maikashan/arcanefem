﻿// -*- tab-width: 2; indent-tabs-mode: nil; coding: utf-8-with-signature -*-
//-----------------------------------------------------------------------------
// Copyright 2000-2023 CEA (www.cea.fr) IFPEN (www.ifpenergiesnouvelles.com)
// See the top-level COPYRIGHT file for details.
// SPDX-License-Identifier: Apache-2.0
//-----------------------------------------------------------------------------
/*---------------------------------------------------------------------------*/
/* BlcsrBiliAssembly.hxx                                     (C) 2022-2023   */
/*                                                                           */
/* Methods of the bilinear assembly phase using the csr data structure       */
/* which avoid to add in the global matrix by iterating through the node.    */
/* It supports GPU Parallelization                                           */
/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void FemModule::_buildMatrixBuildLessCsr()
{

  auto node_dof(m_dofs_on_nodes.nodeDoFConnectivityView());

  // Compute the number of nnz and initialize the memory space
  Integer nbnde = nbNode();
  Int32 nnz = nbFace() * 2 + nbnde;
  m_csr_matrix.initialize(m_dof_family, nnz, nbnde);

  Integer index = 1;
  m_csr_matrix.m_matrix_row(0) = 0;
  ENUMERATE_NODE (inode, allNodes()) {

    Node node = *inode;
    if (index < nbnde) {
      m_csr_matrix.m_matrix_row(index) = node.nbFace() + m_csr_matrix.m_matrix_row(index - 1) + 1;
      index++;
    }
  }
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void FemModule::_buildMatrixGpuBuildLessCsr()
{

  // Compute the number of nnz and initialize the memory space
  Integer nbnde = nbNode();
  Int32 nnz = nbFace() * 2 + nbnde;

  NumArray<Int32, MDDim1> tmp_row;
  tmp_row.resize(nbnde);
  m_csr_matrix.initialize(m_dof_family, nnz, nbnde);

  RunQueue* queue = acceleratorMng()->defaultQueue();
  auto command = makeCommand(queue);
  auto in_out_tmp_row = ax::viewInOut(command, tmp_row);
  auto node_dof(m_dofs_on_nodes.nodeDoFConnectivityView());
  UnstructuredMeshConnectivityView connectivity_view;
  connectivity_view.setMesh(this->mesh());
  auto nfc = connectivity_view.nodeFace();

  command << RUNCOMMAND_ENUMERATE(Node, inode, allNodes())
  {
    Int64 index = node_dof.dofId(inode, 0).localId();
    in_out_tmp_row(index) = nfc.nbFace(inode) + 1;
  };
  ax::Scanner<Int32> scanner;
  scanner.exclusiveSum(queue, tmp_row, m_csr_matrix.m_matrix_row);
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/
ARCCORE_HOST_DEVICE
Real FemModule::_computeCellMatrixGpuTRIA3(CellLocalId icell, IndexedCellNodeConnectivityView cnc, ax::VariableNodeReal3InView in_node_coord, Real b_matrix[6])
{
  Real3 m0 = in_node_coord[cnc.nodeId(icell, 0)];
  Real3 m1 = in_node_coord[cnc.nodeId(icell, 1)];
  Real3 m2 = in_node_coord[cnc.nodeId(icell, 2)];

  Real area = _computeAreaTriangle3Gpu(icell, cnc, in_node_coord);

  Real2 dPhi0(m1.y - m2.y, m2.x - m1.x);
  Real2 dPhi1(m2.y - m0.y, m0.x - m2.x);
  Real2 dPhi2(m0.y - m1.y, m1.x - m0.x);

  Real mul = (1.0 / (2.0 * area));
  b_matrix[0] = dPhi0.x * mul;
  b_matrix[1] = dPhi0.y * mul;

  b_matrix[2] = dPhi1.x * mul;
  b_matrix[3] = dPhi1.y * mul;

  b_matrix[4] = dPhi2.x * mul;
  b_matrix[5] = dPhi2.y * mul;

  return area;
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

ARCCORE_HOST_DEVICE
void FemModule::_addValueToGlobalMatrixTria3Gpu(Int32 begin, Int32 end, Int32 col, ax::NumArrayView<DataViewGetterSetter<Int32>, MDDim1, DefaultLayout> in_out_col_csr, ax::NumArrayView<DataViewGetterSetter<Real>, MDDim1, DefaultLayout> in_out_val_csr, Real x)
{

  // Find the right index in the csr matrix
  while (begin < end) {
    if (in_out_col_csr[begin] == -1) {
      in_out_col_csr[begin] = col;
      in_out_val_csr[begin] = x;
      break;
    }
    else if (in_out_col_csr[begin] == col) {
      in_out_val_csr[begin] += x;
      break;
    }
    begin++;
  }
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void FemModule::_assembleBuildLessCsrBilinearOperatorTria3()
{
  Timer::Action timer_blcsr_bili(m_time_stats, "AssembleBuildLessCsrBilinearOperatorTria3");

  /*
  {
    Timer::Action timer_blcsr_build(m_time_stats, "BuildLessCsrBuildMatrix");
    // Build only the row part of the csr matrix on CPU
    _buildMatrixBuildLessCsr();
  }
*/
  {
    Timer::Action timer_blcsr_build(m_time_stats, "BuildLessCsrBuildMatrixGPU");
    // Build only the row part of the csr matrix on GPU
    // Using scan -> might be improved
    _buildMatrixGpuBuildLessCsr();
  }

  RunQueue* queue = acceleratorMng()->defaultQueue();

  // Boucle sur les noeuds déportée sur accélérateur
  auto command = makeCommand(queue);

  auto node_dof(m_dofs_on_nodes.nodeDoFConnectivityView());
  auto in_row_csr = ax::viewIn(command, m_csr_matrix.m_matrix_row);
  Int32 row_csr_size = m_csr_matrix.m_matrix_row.dim1Size();
  auto in_out_col_csr = ax::viewInOut(command, m_csr_matrix.m_matrix_column);
  Int32 col_csr_size = m_csr_matrix.m_matrix_column.dim1Size();
  auto in_out_val_csr = ax::viewInOut(command, m_csr_matrix.m_matrix_value);

  auto in_node_coord = ax::viewIn(command, m_node_coord);

  UnstructuredMeshConnectivityView m_connectivity_view;
  m_connectivity_view.setMesh(this->mesh());
  auto ncc = m_connectivity_view.nodeCell();
  auto cnc = m_connectivity_view.cellNode();
  Arcane::ItemGenericInfoListView nodes_infos(this->mesh()->nodeFamily());
  Arcane::ItemGenericInfoListView cells_infos(this->mesh()->cellFamily());

  Timer::Action timer_blcsr_add_compute(m_time_stats, "BuildLessCsrAddAndCompute");
  command << RUNCOMMAND_ENUMERATE(Node, inode, allNodes())
  {
    Int32 inode_index = 0;
    for (auto cell : ncc.cells(inode)) {

      // How can I know the right index ?
      // By checking in the global id ?
      // Working currently, but maybe only because p = 1 ?
      if (inode == cnc.nodeId(cell, 1)) {
        inode_index = 1;
      }
      else if (inode == cnc.nodeId(cell, 2)) {
        inode_index = 2;
      }
      else {
        inode_index = 0;
      }

      Real b_matrix[6] = { 0 };
      Real area = _computeCellMatrixGpuTRIA3(cell, cnc, in_node_coord, b_matrix);

      Int32 i = 0;
      for (NodeLocalId node2 : cnc.nodes(cell)) {
        Real x = 0.0;
        for (Int32 k = 0; k < 2; k++) {
          x += b_matrix[inode_index * 2 + k] * b_matrix[i * 2 + k];
        }
        x = x * area;
        if (nodes_infos.isOwn(inode)) {

          Int32 row = node_dof.dofId(inode, 0).localId();
          Int32 col = node_dof.dofId(node2, 0).localId();
          Int32 begin = in_row_csr[row];
          Int32 end;
          if (row == row_csr_size - 1) {
            end = col_csr_size;
          }
          else {
            end = in_row_csr[row + 1];
          }
          _addValueToGlobalMatrixTria3Gpu(begin, end, col, in_out_col_csr, in_out_val_csr, x);
        }
        i++;
      }
    }
  };
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/
