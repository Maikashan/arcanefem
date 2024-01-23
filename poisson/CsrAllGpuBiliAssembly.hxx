// -*- tab-width: 2; indent-tabs-mode: nil; coding: utf-8-with-signature -*-
//-----------------------------------------------------------------------------
// Copyright 2000-2023 CEA (www.cea.fr) IFPEN (www.ifpenergiesnouvelles.com)
// See the top-level COPYRIGHT file for details.
// SPDX-License-Identifier: Apache-2.0
//-----------------------------------------------------------------------------
/*---------------------------------------------------------------------------*/
/* CsrGpuiBiliAssembly.hxx                                     (C) 2022-2023 */
/*                                                                           */
/* Methods of the bilinear assembly phase using the csr data structure       */
/* which handle the parallelization on Nvidia GPU                            */
/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void FemModule::
_assembleCsrAllGPUBilinearOperatorTRIA3()
{

  Timer::Action timer_gpu_bili(m_time_stats, "AssembleCsrAllGpuBilinearOperatorTria3");

  std::chrono::_V2::system_clock::time_point lhs_start;
  double global_build_average = 0;
  double build_time = 0;
  if (m_register_time) {
    logger << "-------------------------------------------------------------------------------------\n"
           << "Using GPU csr with NumArray format\n";
    lhs_start = std::chrono::high_resolution_clock::now();
  }
  {
    Timer::Action timer_gpu_build(m_time_stats, "CsrAllGpuBuildMatrix");
    // Build the csr matrix with the function from BlCsrBiliAssembly.hxx
    _buildMatrixGpuBuildLessCsr();
    m_csr_matrix.printMatrix("test.txt");
  }

  std::chrono::_V2::system_clock::time_point build_stop;
  std::chrono::_V2::system_clock::time_point var_init_start;
  if (m_register_time) {
    build_stop = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> build_duration = build_stop - lhs_start;
    build_time = build_duration.count();
    var_init_start = std::chrono::high_resolution_clock::now();
  }

  RunQueue* queue = acceleratorMng()->defaultQueue();
  // Boucle sur les mailles déportée sur accélérateur
  auto command = makeCommand(queue);

  auto node_dof(m_dofs_on_nodes.nodeDoFConnectivityView());
  auto in_row_csr = ax::viewIn(command, m_csr_matrix.m_matrix_row);
  Int32 row_csr_size = m_csr_matrix.m_matrix_row.dim1Size();
  auto in_out_col_csr = ax::viewInOut(command, m_csr_matrix.m_matrix_column);
  Int32 col_csr_size = m_csr_matrix.m_matrix_column.dim1Size();
  auto in_out_val_csr = ax::viewInOut(command, m_csr_matrix.m_matrix_value);
  UnstructuredMeshConnectivityView m_connectivity_view;
  auto in_node_coord = ax::viewIn(command, m_node_coord);
  m_connectivity_view.setMesh(this->mesh());
  auto cnc = m_connectivity_view.cellNode();
  Arcane::ItemGenericInfoListView nodes_infos(this->mesh()->nodeFamily());
  Arcane::ItemGenericInfoListView cells_infos(this->mesh()->cellFamily());

  std::chrono::_V2::system_clock::time_point var_init_stop;
  std::chrono::_V2::system_clock::time_point loop_start;
  double var_init_time = 0;
  if (m_register_time) {
    var_init_stop = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> var_init_duration = var_init_stop - var_init_start;
    var_init_time = var_init_duration.count();
    loop_start = std::chrono::high_resolution_clock::now();
  }

  Timer::Action timer_add_compute(m_time_stats, "CsrAllGpuAddComputeLoop");

  command << RUNCOMMAND_ENUMERATE(Cell, icell, allCells())
  {

    Real K_e[9] = { 0 };

    _computeElementMatrixTRIA3GPU(icell, cnc, in_node_coord, K_e); // element stifness matrix

    //             # assemble elementary matrix into the global one
    //             # elementary terms are positionned into K according
    //             # to the rank of associated node in the mesh.nodes list
    //             for node1 in elem.nodes:
    //                 inode1=elem.nodes.index(node1) # get position of node1 in nodes list
    //                 for node2 in elem.nodes:
    //                     inode2=elem.nodes.index(node2)
    //                     K[node1.rank,node2.rank]=K[node1.rank,node2.rank]+K_e[inode1,inode2]
    Int32 n1_index = 0;
    for (NodeLocalId node1 : cnc.nodes(icell)) {
      Int32 n2_index = 0;
      for (NodeLocalId node2 : cnc.nodes(icell)) {
        // K[node1.rank,node2.rank]=K[node1.rank,node2.rank]+K_e[inode1,inode2]
        double v = K_e[n1_index * 3 + n2_index];
        // m_k_matrix(node1.localId(), node2.localId()) += v;
        if (nodes_infos.isOwn(node1)) {
          Int32 row = node_dof.dofId(node1, 0).localId();
          Int32 col = node_dof.dofId(node2, 0).localId();
          Int32 begin = in_row_csr[row];
          Int32 end;
          if (row == row_csr_size - 1) {
            end = col_csr_size;
          }
          else {
            end = in_row_csr[row + 1];
          }
          while (begin < end) {
            if (in_out_col_csr[begin] == col) {
              // t is necessary to get the right type for the atomicAdd (but that means that we have more operations ?)
              // The Macro is there to avoid compilation error if not in c++ 20
              ax::doAtomic<ax::eAtomicOperation::Add>(in_out_val_csr(begin), v);
              break;
            }
            else if (in_out_col_csr[begin] == -1) {
              in_out_col_csr[begin] = col;
              in_out_val_csr(begin) = v;
              break;
            }
            begin++;
          }
        }
        ++n2_index;
      }
      ++n1_index;
    }
  };
  if (m_register_time) {
    auto lhs_end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = lhs_end - lhs_start;
    std::chrono::duration<double> loop_duration = lhs_end - loop_start;

    double loop_time = loop_duration.count();
    double lhs_loc_time = duration.count();
    logger << "Building time of the coo matrix :" << build_time << "\n"
           << "Variable initialisation time : " << var_init_time << "\n"
           << "Computation and Addition time : " << loop_time << "\n"
           << "LHS Total time : " << lhs_loc_time << "\n"
           << "Build matrix time in lhs :" << build_time / lhs_loc_time * 100 << "%\n"
           << "Variable initialisation time in lhs : " << var_init_time / lhs_loc_time * 100 << "%\n"
           << "Computation and Addition time in lhs : " << loop_time / lhs_loc_time * 100 << "%\n\n"
           << "-------------------------------------------------------------------------------------\n\n";
    lhs_time += lhs_loc_time;
    wbuild << lhs_loc_time << ",";
    timer << loop_time << ",";
  }
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/