﻿// -*- tab-width: 2; indent-tabs-mode: nil; coding: utf-8-with-signature -*-
//-----------------------------------------------------------------------------
// Copyright 2000-2023 CEA (www.cea.fr) IFPEN (www.ifpenergiesnouvelles.com)
// See the top-level COPYRIGHT file for details.
// SPDX-License-Identifier: Apache-2.0
//-----------------------------------------------------------------------------
/*---------------------------------------------------------------------------*/
/* FemModule.cc                                                (C) 2022-2023 */
/*                                                                           */
/* Simple module to test simple FEM mechanism.                               */
/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/
#include "FemModule.h"

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void FemModule::
_writeInJson()
{
  ofstream jsonFile("time.json");
  JSONWriter json_writer(JSONWriter::FormatFlags::None);
  json_writer.beginObject();
  JSONWriter::Object jo(json_writer, "TimerToTo");

  this->subDomain()->timeStats()->dumpStatsJSON(json_writer);
  json_writer.endObject();
  jsonFile << json_writer.getBuffer();
}

void FemModule::
endModule()
{
  _writeInJson();
}

void FemModule::
compute()
{
  info() << "Module Fem COMPUTE";

  // Stop code after computations
  if (m_global_iteration() > 0)
    subDomain()->timeLoopMng()->stopComputeLoop(true);

  m_linear_system.reset();
  m_linear_system.setLinearSystemFactory(options()->linearSystem());

  m_linear_system.initialize(subDomain(), m_dofs_on_nodes.dofFamily(), "Solver");
  // Test for adding parameters for PETSc.
  // This is only used for the first call.
  {
    StringList string_list;
    string_list.add("-trmalloc");
    string_list.add("-log_trace");

    string_list.add("-ksp_monitor");
    string_list.add("-ksp_view");
    string_list.add("-math_view");
    string_list.add("draw");
    string_list.add("-draw_pause");
    string_list.add("-10");

    CommandLineArguments args(string_list);
    m_linear_system.setSolverCommandLineArguments(args);
  }
  info() << "NB_CELL=" << allCells().size() << " NB_FACE=" << allFaces().size();

  _doStationarySolve();
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void FemModule::
startInit()
{
  info() << "Module Fem INIT";

  if (m_register_time) {
    logger = ofstream("timer.txt");
    wbuild = ofstream("with_build.csv", std::ios_base::app);
    wbuild << nbNode() << ",";
    timer = ofstream("timer.csv", std::ios_base::app);
    timer << nbNode() << ",";
  }

  m_dofs_on_nodes.initialize(mesh(), 1);
  m_dof_family = m_dofs_on_nodes.dofFamily();

  //_buildDoFOnNodes();
  //Int32 nb_node = allNodes().size();
  //m_k_matrix.resize(nb_node, nb_node);
  //m_k_matrix.fill(0.0);

  //m_rhs_vector.resize(nb_node);
  //m_rhs_vector.fill(0.0);

  // # init mesh
  // # init behavior
  // # init behavior on mesh entities
  // # init BCs
  _handleFlags();
  _initBoundaryconditions();

  _checkCellType();
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void FemModule ::
_handleFlags()
{
  ParameterList parameter_list = this->subDomain()->application()->applicationInfo().commandLineArguments().parameters();
  info() << "-----------------------------------------------------------------------------------------";
  info() << "The time will be registered by arcane in the output/listing/logs.0 file";
  if (parameter_list.getParameterOrNull("REGISTER_TIME") == "TRUE") {
    m_register_time = true;
    info() << "REGISTER_TIME: The time will also be registered in the timer.txt, with_build.csv and timer.csv file";
  }
  if (parameter_list.getParameterOrNull("CACHE_WARMING") != NULL) {
    std::stringstream test("");
    test << parameter_list.getParameterOrNull("CACHE_WARMING");
    m_cache_warming = std::stoi(test.str());
    info() << "CACHE_WARMING: A cache warming of " << m_cache_warming << " iterations will happen";
  }
  if (parameter_list.getParameterOrNull("COO") == "TRUE") {
    m_use_coo = true;
    m_use_legacy = false;
    info() << "COO: The COO datastructure and its associated methods will be used";
  }
  if (parameter_list.getParameterOrNull("COO_SORT") == "TRUE") {
    m_use_coo_sort = true;
    m_use_legacy = false;
    info() << "COO_SORT: The COO with sorting datastructure and its associated methods will be used";
  }
  if (parameter_list.getParameterOrNull("CSR") == "TRUE") {
    m_use_csr = true;
    m_use_legacy = false;
    info() << "CSR: The CSR datastructure and its associated methods will be used";
  }
#ifdef ARCANE_HAS_CUDA
  if (parameter_list.getParameterOrNull("CSR_GPU") == "TRUE") {
    m_use_csr_gpu = true;
    m_use_legacy = false;
    info() << "CSR_GPU: The CSR datastructure GPU compatible and its associated methods will be used";
  }
  if (parameter_list.getParameterOrNull("NWCSR") == "TRUE") {
    m_use_nodewise_csr = true;
    m_use_legacy = false;
    info() << "NWCSR: The Csr datastructure (GPU compatible) and its associated methods will be used with computation in a nodewise manner";
  }
  if (parameter_list.getParameterOrNull("BLCSR") == "TRUE") {
    m_use_buildless_csr = true;
    m_use_legacy = false;
    info() << "BLCSR: The Csr datastructure (GPU compatible) and its associated methods will be used with computation in a nodewise manner with the building phases incorporated in the computation";
  }
  if (parameter_list.getParameterOrNull("CUSPARSE_ADD") == "TRUE") {
    m_use_cusparse_add = true;
    m_use_legacy = false;
    info() << "CUSPARSE_ADD: CUSPARSE and its associated methods will be used";
  }
#endif
  if (parameter_list.getParameterOrNull("LEGACY") == "TRUE") {
    m_use_legacy = true;
    info() << "LEGACY: The Legacy datastructure and its associated methods will be used";
  }
  else if (parameter_list.getParameterOrNull("LEGACY") == "FALSE") {
    m_use_legacy = false;
  }
  info() << "-----------------------------------------------------------------------------------------";
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void FemModule::
_doStationarySolve()
{
  Timer::Action timer_action(this->subDomain(), "StationarySolve");
  std::chrono::_V2::system_clock::time_point fem_start;
  if (m_register_time && m_cache_warming)
    fem_start = std::chrono::high_resolution_clock::now();

  // # get material parameters
  _getMaterialParameters();

  // # update BCs
  _updateBoundayConditions();

  // Assemble the FEM bilinear operator (LHS - matrix A)
  if (options()->meshType == "QUAD4")
    _assembleBilinearOperatorQUAD4();
  else {

#ifdef ARCANE_HAS_CUDA
    if (m_use_cusparse_add) {
      for (cache_index = 0; cache_index < m_cache_warming; cache_index++) {
        _assembleCusparseBilinearOperatorTRIA3();
      }
    }

#endif
    if (m_use_coo) {
      for (cache_index = 0; cache_index < m_cache_warming; cache_index++) {
        m_linear_system.clearValues();
        _assembleCooBilinearOperatorTRIA3();
      }
      m_coo_matrix.translateToLinearSystem(m_linear_system);
    }
    if (m_use_coo_sort) {
      for (cache_index = 0; cache_index < m_cache_warming; cache_index++) {
        m_linear_system.clearValues();
        _assembleCooSortBilinearOperatorTRIA3();
      }
      m_coo_matrix.translateToLinearSystem(m_linear_system);
    }
#ifdef USE_COO_GPU
#ifdef m_cache_warming
    for (i = 0; i < 3; i++)
#endif
    {
      m_linear_system.clearValues();
      _assembleCooGPUBilinearOperatorTRIA3();
    }
    m_coo_matrix.translateToLinearSystem(m_linear_system);
#endif
    if (m_use_csr) {
      for (cache_index = 0; cache_index < m_cache_warming; cache_index++) {
        m_linear_system.clearValues();
        _assembleCsrBilinearOperatorTRIA3();
      }
      m_csr_matrix.translateToLinearSystem(m_linear_system);
    }
    if (m_use_legacy) {
      for (cache_index = 0; cache_index < m_cache_warming; cache_index++) {
        m_linear_system.clearValues();
        _assembleBilinearOperatorTRIA3();
      }
    }

#ifdef ARCANE_HAS_CUDA
    if (m_use_csr_gpu) {
      for (cache_index = 0; cache_index < m_cache_warming; cache_index++) {
        m_linear_system.clearValues();
        _assembleCsrGPUBilinearOperatorTRIA3();
      }
      m_csr_matrix.translateToLinearSystem(m_linear_system);
    }
    if (m_use_nodewise_csr) {
      for (cache_index = 0; cache_index < m_cache_warming; cache_index++) {
        m_linear_system.clearValues();
        _assembleNodeWiseCsrBilinearOperatorTria3();
      }
      m_csr_matrix.translateToLinearSystem(m_linear_system);
    }
    if (m_use_buildless_csr) {
      for (cache_index = 0; cache_index < m_cache_warming; cache_index++) {
        m_linear_system.clearValues();
        _assembleBuildLessCsrBilinearOperatorTria3();
      }
      m_csr_matrix.translateToLinearSystem(m_linear_system);
    }
  }
#endif
  // Assemble the FEM linear operator (RHS - vector b)
  if (m_use_buildless_csr) {
    m_linear_system.clearValues();
    _assembleCsrLinearOperator();
    m_csr_matrix.translateToLinearSystem(m_linear_system);
    _translateRhs();
  }
  else {
    _assembleLinearOperator();
  }

  // # T=linalg.solve(K,RHS)
  _solve();

  // Check results
  _checkResultFile();

  if (m_register_time) {
    auto fem_stop = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> fem_duration = fem_stop - fem_start;
    double total_duration = fem_duration.count();
    logger << "FEM total duration : " << fem_duration.count() << "\n"
           << "LHS time in total duration : " << lhs_time / total_duration * 100 << "%\n"
           << "RHS time in total duration : " << rhs_time / total_duration * 100 << "%\n"
           << "Solver time in total duration : " << solver_time / total_duration * 100 << "%\n";
    logger.close();
  }
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void FemModule::
_getMaterialParameters()
{
  info() << "Get material parameters...";
  f = options()->f();
  ElementNodes = 3.;

  if (options()->meshType == "QUAD4")
    ElementNodes = 4.;
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void FemModule::
_initBoundaryconditions()
{
  info() << "Init boundary conditions...";

  info() << "Apply boundary conditions";
  _applyDirichletBoundaryConditions();
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void FemModule::
_applyDirichletBoundaryConditions()
{
  // Handle all the Dirichlet boundary conditions.
  // In the 'arc' file, there are in the following format:
  //   <dirichlet-boundary-condition>
  //   <surface>Haut</surface>
  //   <value>21.0</value>
  // </dirichlet-boundary-condition>

  for (const auto& bs : options()->dirichletBoundaryCondition()) {
    FaceGroup group = bs->surface();
    Real value = bs->value();
    info() << "Apply Dirichlet boundary condition surface=" << group.name() << " v=" << value;
    ENUMERATE_ (Face, iface, group) {
      for (Node node : iface->nodes()) {
        m_u[node] = value;
        m_u_dirichlet[node] = true;
      }
    }
  }

  for (const auto& bs : options()->dirichletPointCondition()) {
    NodeGroup group = bs->node();
    Real value = bs->value();
    info() << "Apply Dirichlet point condition node=" << group.name() << " v=" << value;
    ENUMERATE_ (Node, inode, group) {
      Node node = *inode;
      m_u[node] = value;
      m_u_dirichlet[node] = true;
    }
  }
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void FemModule::
_checkCellType()
{
  Int16 type = 0;
  if (options()->meshType == "QUAD4") {
    type = IT_Quad4;
  }
  else {
    type = IT_Triangle3;
  }
  ENUMERATE_ (Cell, icell, allCells()) {
    Cell cell = *icell;
    if (cell.type() != type)
      ARCANE_FATAL("Only Triangle3 cell type is supported");
  }
}

void FemModule::
_updateBoundayConditions()
{
  info() << "TODO " << A_FUNCINFO;
}

/*---------------------------------------------------------------------------*/
// Assemble the FEM linear operator
//  - This function enforces a Dirichlet boundary condition in a weak sense
//    via the penalty method
//  - The method also adds source term
//  - TODO: external fluxes
/*---------------------------------------------------------------------------*/

void FemModule::
_assembleLinearOperator()
{
  info() << "Assembly of FEM linear operator ";
  info() << "Applying Dirichlet boundary condition via  penalty method ";

  // time registration
  std::chrono::_V2::system_clock::time_point rhs_start;
  double penalty_time = 0;
  double wpenalty_time = 0;
  double sassembly_time = 0;
  double fassembly_time = 0;
  if (m_register_time) {
    rhs_start = std::chrono::high_resolution_clock::now();
  }

  Timer::Action timer_action(this->subDomain(), "AssembleLinearOperator");

  // Temporary variable to keep values for the RHS part of the linear system
  VariableDoFReal& rhs_values(m_linear_system.rhsVariable());
  rhs_values.fill(0.0);

  auto node_dof(m_dofs_on_nodes.nodeDoFConnectivityView());

  if (options()->enforceDirichletMethod() == "Penalty") {

    Timer::Action timer_action(this->subDomain(), "Penalty");

    //----------------------------------------------
    // penalty method to enforce Dirichlet BC
    //----------------------------------------------
    //  Let 'P' be the penalty term and let 'i' be the set of DOF for which
    //  Dirichlet condition needs to be applied
    //
    //  - For LHS matrix A the diag term corresponding to the Dirichlet DOF
    //           a_{i,i} = 1. * P
    //
    //  - For RHS vector b the term that corresponds to the Dirichlet DOF
    //           b_{i} = b_{i} * P
    //----------------------------------------------

    info() << "Applying Dirichlet boundary condition via "
           << options()->enforceDirichletMethod() << " method ";

    Real Penalty = options()->penalty(); // 1.0e30 is the default
    std::chrono::_V2::system_clock::time_point penalty_start;
    if (m_register_time) {
      penalty_start = std::chrono::high_resolution_clock::now();
    }

    ENUMERATE_ (Node, inode, ownNodes()) {
      NodeLocalId node_id = *inode;
      if (m_u_dirichlet[node_id]) {
        DoFLocalId dof_id = node_dof.dofId(*inode, 0);
        // This SetValue should be updated in the acoording format we have (such as COO or CSR)
        m_linear_system.matrixSetValue(dof_id, dof_id, Penalty);
        Real u_g = Penalty * m_u[node_id];
        // This should be changed for a numArray
        rhs_values[dof_id] = u_g;
      }
    }
    std::chrono::_V2::system_clock::time_point penalty_stop;
    if (m_register_time) {
      penalty_stop = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double> penalty_duration = penalty_stop - penalty_start;
      penalty_time = penalty_duration.count();
      logger << "Penalty duration : " << penalty_time << "\n";
    }
  }
  else if (options()->enforceDirichletMethod() == "WeakPenalty") {
    Timer::Action timer_action(this->subDomain(), "WeakPenalty");

    //----------------------------------------------
    // weak penalty method to enforce Dirichlet BC
    //----------------------------------------------
    //  Let 'P' be the penalty term and let 'i' be the set of DOF for which
    //  Dirichlet condition needs to be applied
    //
    //  - For LHS matrix A the diag term corresponding to the Dirichlet DOF
    //           a_{i,i} = a_{i,i} + P
    //
    //  - For RHS vector b the term that corresponds to the Dirichlet DOF
    //           b_{i} = b_{i} * P
    //----------------------------------------------

    info() << "Applying Dirichlet boundary condition via "
           << options()->enforceDirichletMethod() << " method ";

    Real Penalty = options()->penalty(); // 1.0e30 is the default
    std::chrono::_V2::system_clock::time_point wpenalty_start;
    if (m_register_time) {
      wpenalty_start = std::chrono::high_resolution_clock::now();
    }

    // The same as before
    ENUMERATE_ (Node, inode, ownNodes()) {
      NodeLocalId node_id = *inode;
      if (m_u_dirichlet[node_id]) {
        DoFLocalId dof_id = node_dof.dofId(*inode, 0);
        m_linear_system.matrixAddValue(dof_id, dof_id, Penalty);
        Real u_g = Penalty * m_u[node_id];
        rhs_values[dof_id] = u_g;
      }
    }
    std::chrono::_V2::system_clock::time_point wpenalty_stop;
    if (m_register_time) {
      wpenalty_stop = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double> wpenalty_duration = wpenalty_stop - wpenalty_start;
      wpenalty_time = wpenalty_duration.count();
      logger << "Weak Penalty duration : " << wpenalty_time << "\n";
    }
  }
  else if (options()->enforceDirichletMethod() == "RowElimination") {

    //----------------------------------------------
    // Row elimination method to enforce Dirichlet BC
    //----------------------------------------------
    //  Let 'I' be the set of DOF for which  Dirichlet condition needs to be applied
    //
    //  to apply the Dirichlet on 'i'th DOF
    //  - For LHS matrix A the row terms corresponding to the Dirichlet DOF
    //           a_{i,j} = 0.  : i!=j
    //           a_{i,j} = 1.  : i==j
    //----------------------------------------------

    info() << "Applying Dirichlet boundary condition via "
           << options()->enforceDirichletMethod() << " method ";
    // The same as before
    // TODO
  }
  else if (options()->enforceDirichletMethod() == "RowColumnElimination") {

    //----------------------------------------------
    // Row elimination method to enforce Dirichlet BC
    //----------------------------------------------
    //  Let 'I' be the set of DOF for which  Dirichlet condition needs to be applied
    //
    //  to apply the Dirichlet on 'i'th DOF
    //  - For LHS matrix A the row terms corresponding to the Dirichlet DOF
    //           a_{i,j} = 0.  : i!=j  for all j
    //           a_{i,j} = 1.  : i==j
    //    also the column terms corresponding to the Dirichlet DOF
    //           a_{i,j} = 0.  : i!=j  for all i
    //----------------------------------------------

    info() << "Applying Dirichlet boundary condition via "
           << options()->enforceDirichletMethod() << " method ";

    // The same as before
    // TODO
  }
  else {

    info() << "Applying Dirichlet boundary condition via "
           << options()->enforceDirichletMethod() << " is not supported \n"
           << "enforce-Dirichlet-method only supports:\n"
           << "  - Penalty\n"
           << "  - WeakPenalty\n"
           << "  - RowElimination\n"
           << "  - RowColumnElimination\n";
  }
  std::chrono::_V2::system_clock::time_point sassemby_start;
  if (m_register_time) {
    sassemby_start = std::chrono::high_resolution_clock::now();
  }

  std::chrono::_V2::system_clock::time_point fassemby_start;
  {
    Timer::Action timer_action(this->subDomain(), "ConstantSourceTermAssembly");
    //----------------------------------------------
    // Constant source term assembly
    //----------------------------------------------
    //
    //  $int_{Omega}(f*v^h)$
    //  only for noded that are non-Dirichlet
    //----------------------------------------------
    ENUMERATE_ (Cell, icell, allCells()) {
      Cell cell = *icell;
      Real area = _computeAreaTriangle3(cell);
      for (Node node : cell.nodes()) {
        if (!(m_u_dirichlet[node]) && node.isOwn())
          // must replace rhs_values with numArray
          rhs_values[node_dof.dofId(node, 0)] += f * area / ElementNodes;
      }
    }
    std::chrono::_V2::system_clock::time_point sassemby_stop;
    if (m_register_time) {
      sassemby_stop = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double> sassembly_duration = sassemby_stop - sassemby_start;
      sassembly_time = sassembly_duration.count();
      logger << "Constant source term assembly duration : " << sassembly_time << "\n";
      fassemby_start = std::chrono::high_resolution_clock::now();
    }
  }
  {
    Timer::Action timer_action(this->subDomain(), "ConstantSourceTermAssembly");

    //----------------------------------------------
    // Constant flux term assembly
    //----------------------------------------------
    //
    //  only for noded that are non-Dirichlet
    //  $int_{dOmega_N}((q.n)*v^h)$
    // or
    //  $int_{dOmega_N}((n_x*q_x + n_y*q_y)*v^h)$
    //----------------------------------------------
    for (const auto& bs : options()->neumannBoundaryCondition()) {
      FaceGroup group = bs->surface();

      if (bs->value.isPresent()) {
        Real value = bs->value();
        ENUMERATE_ (Face, iface, group) {
          Face face = *iface;
          Real length = _computeEdgeLength2(face);
          for (Node node : iface->nodes()) {
            if (!(m_u_dirichlet[node]) && node.isOwn())
              // must replace rhs_values with numArray
              rhs_values[node_dof.dofId(node, 0)] += value * length / 2.;
          }
        }
        continue;
      }

      if (bs->valueX.isPresent() && bs->valueY.isPresent()) {
        Real valueX = bs->valueX();
        Real valueY = bs->valueY();
        ENUMERATE_ (Face, iface, group) {
          Face face = *iface;
          Real length = _computeEdgeLength2(face);
          Real2 Normal = _computeEdgeNormal2(face);
          for (Node node : iface->nodes()) {
            if (!(m_u_dirichlet[node]) && node.isOwn())
              // must replace rhs_values with numArray
              rhs_values[node_dof.dofId(node, 0)] += (Normal.x * valueX + Normal.y * valueY) * length / 2.;
          }
        }
        continue;
      }

      if (bs->valueX.isPresent()) {
        Real valueX = bs->valueX();
        ENUMERATE_ (Face, iface, group) {
          Face face = *iface;
          Real length = _computeEdgeLength2(face);
          Real2 Normal = _computeEdgeNormal2(face);
          for (Node node : iface->nodes()) {
            if (!(m_u_dirichlet[node]) && node.isOwn())
              // must replace rhs_values with numArray
              rhs_values[node_dof.dofId(node, 0)] += (Normal.x * valueX) * length / 2.;
          }
        }
        continue;
      }

      if (bs->valueY.isPresent()) {
        Real valueY = bs->valueY();
        ENUMERATE_ (Face, iface, group) {
          Face face = *iface;
          Real length = _computeEdgeLength2(face);
          Real2 Normal = _computeEdgeNormal2(face);
          for (Node node : iface->nodes()) {
            if (!(m_u_dirichlet[node]) && node.isOwn())
              // must replace rhs_values with numArray
              rhs_values[node_dof.dofId(node, 0)] += (Normal.y * valueY) * length / 2.;
          }
        }
        continue;
      }
    }
    std::chrono::_V2::system_clock::time_point fassemby_stop;
    if (m_register_time) {

      fassemby_stop = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double> fassembly_duration = fassemby_stop - fassemby_start;
      fassembly_time = fassembly_duration.count();
      logger << "Constant flux term assembly duration : " << fassembly_time << "\n";
      auto rhs_end = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double> duration = rhs_end - rhs_start;
      rhs_time = duration.count();
      logger << "RHS total duration : " << duration.count() << "\n";
      if (penalty_time != 0)
        logger << "Penalty time in rhs : " << penalty_time / rhs_time * 100 << "%\n";
      else
        logger << "Weak Penalty time in rhs : " << wpenalty_time / rhs_time * 100 << "%\n";
      logger << "Constant source term assembly time in rhs : " << sassembly_time / rhs_time * 100 << "%\n"
             << "Constant flux term assembly time in rhs : " << fassembly_time / rhs_time * 100 << "%\n\n"
             << "-------------------------------------------------------------------------------------\n\n";
    }
  }
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void FemModule::
_assembleCsrLinearOperator()
{
  info() << "Assembly of FEM linear operator ";
  info() << "Applying Dirichlet boundary condition via  penalty method for Csr";

  // time registration
  std::chrono::_V2::system_clock::time_point rhs_start;
  double penalty_time = 0;
  double wpenalty_time = 0;
  double sassembly_time = 0;
  double fassembly_time = 0;
  if (m_register_time) {
    rhs_start = std::chrono::high_resolution_clock::now();
  }

  Timer::Action timer_action(this->subDomain(), "CsrAssembleLinearOperator");

  m_rhs_vect.resize(nbNode());
  m_rhs_vect.fill(0.0);

  auto node_dof(m_dofs_on_nodes.nodeDoFConnectivityView());

  if (options()->enforceDirichletMethod() == "Penalty") {

    Timer::Action timer_action(this->subDomain(), "CsrPenalty");

    //----------------------------------------------
    // penalty method to enforce Dirichlet BC
    //----------------------------------------------
    //  Let 'P' be the penalty term and let 'i' be the set of DOF for which
    //  Dirichlet condition needs to be applied
    //
    //  - For LHS matrix A the diag term corresponding to the Dirichlet DOF
    //           a_{i,i} = 1. * P
    //
    //  - For RHS vector b the term that corresponds to the Dirichlet DOF
    //           b_{i} = b_{i} * P
    //----------------------------------------------

    info() << "Applying Dirichlet boundary condition via "
           << options()->enforceDirichletMethod() << " method ";

    Real Penalty = options()->penalty(); // 1.0e30 is the default
    std::chrono::_V2::system_clock::time_point penalty_start;
    if (m_register_time) {
      penalty_start = std::chrono::high_resolution_clock::now();
    }

    ENUMERATE_ (Node, inode, ownNodes()) {
      NodeLocalId node_id = *inode;
      if (m_u_dirichlet[node_id]) {
        DoFLocalId dof_id = node_dof.dofId(*inode, 0);
        // This SetValue should be updated in the acoording format we have (such as COO or CSR)
        m_csr_matrix.matrixSetValue(dof_id, dof_id, Penalty);
        Real u_g = Penalty * m_u[node_id];
        // This should be changed for a numArray
        m_rhs_vect[dof_id] = u_g;
      }
    }
    std::chrono::_V2::system_clock::time_point penalty_stop;
    if (m_register_time) {
      penalty_stop = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double> penalty_duration = penalty_stop - penalty_start;
      penalty_time = penalty_duration.count();
      logger << "Penalty duration for CSR : " << penalty_time << "\n";
    }
  }
  else if (options()->enforceDirichletMethod() == "WeakPenalty") {
    Timer::Action timer_action(this->subDomain(), "CsrWeakPenalty");

    //----------------------------------------------
    // weak penalty method to enforce Dirichlet BC
    //----------------------------------------------
    //  Let 'P' be the penalty term and let 'i' be the set of DOF for which
    //  Dirichlet condition needs to be applied
    //
    //  - For LHS matrix A the diag term corresponding to the Dirichlet DOF
    //           a_{i,i} = a_{i,i} + P
    //
    //  - For RHS vector b the term that corresponds to the Dirichlet DOF
    //           b_{i} = b_{i} * P
    //----------------------------------------------

    info() << "Applying Dirichlet boundary condition via "
           << options()->enforceDirichletMethod() << " method ";

    Real Penalty = options()->penalty(); // 1.0e30 is the default
    std::chrono::_V2::system_clock::time_point wpenalty_start;
    if (m_register_time) {
      wpenalty_start = std::chrono::high_resolution_clock::now();
    }

    // The same as before
    ENUMERATE_ (Node, inode, ownNodes()) {
      NodeLocalId node_id = *inode;
      if (m_u_dirichlet[node_id]) {
        DoFLocalId dof_id = node_dof.dofId(*inode, 0);
        m_csr_matrix.matrixAddValue(dof_id, dof_id, Penalty);
        Real u_g = Penalty * m_u[node_id];
        m_rhs_vect[dof_id] = u_g;
      }
    }
    std::chrono::_V2::system_clock::time_point wpenalty_stop;
    if (m_register_time) {
      wpenalty_stop = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double> wpenalty_duration = wpenalty_stop - wpenalty_start;
      wpenalty_time = wpenalty_duration.count();
      logger << "Weak Penalty duration for CSR: " << wpenalty_time << "\n";
    }
  }
  else if (options()->enforceDirichletMethod() == "RowElimination") {

    //----------------------------------------------
    // Row elimination method to enforce Dirichlet BC
    //----------------------------------------------
    //  Let 'I' be the set of DOF for which  Dirichlet condition needs to be applied
    //
    //  to apply the Dirichlet on 'i'th DOF
    //  - For LHS matrix A the row terms corresponding to the Dirichlet DOF
    //           a_{i,j} = 0.  : i!=j
    //           a_{i,j} = 1.  : i==j
    //----------------------------------------------

    info() << "Applying Dirichlet boundary condition via "
           << options()->enforceDirichletMethod() << " method ";
    // The same as before
    // TODO
  }
  else if (options()->enforceDirichletMethod() == "RowColumnElimination") {

    //----------------------------------------------
    // Row elimination method to enforce Dirichlet BC
    //----------------------------------------------
    //  Let 'I' be the set of DOF for which  Dirichlet condition needs to be applied
    //
    //  to apply the Dirichlet on 'i'th DOF
    //  - For LHS matrix A the row terms corresponding to the Dirichlet DOF
    //           a_{i,j} = 0.  : i!=j  for all j
    //           a_{i,j} = 1.  : i==j
    //    also the column terms corresponding to the Dirichlet DOF
    //           a_{i,j} = 0.  : i!=j  for all i
    //----------------------------------------------

    info() << "Applying Dirichlet boundary condition via "
           << options()->enforceDirichletMethod() << " method ";

    // The same as before
    // TODO
  }
  else {

    info() << "Applying Dirichlet boundary condition via "
           << options()->enforceDirichletMethod() << " is not supported \n"
           << "enforce-Dirichlet-method only supports:\n"
           << "  - Penalty\n"
           << "  - WeakPenalty\n"
           << "  - RowElimination\n"
           << "  - RowColumnElimination\n";
  }
  std::chrono::_V2::system_clock::time_point sassemby_start;
  if (m_register_time) {
    sassemby_start = std::chrono::high_resolution_clock::now();
  }

  std::chrono::_V2::system_clock::time_point fassemby_start;
  {
    Timer::Action timer_action(this->subDomain(), "CsrConstantSourceTermAssembly");
    //----------------------------------------------
    // Constant source term assembly
    //----------------------------------------------
    //
    //  $int_{Omega}(f*v^h)$
    //  only for noded that are non-Dirichlet
    //----------------------------------------------
    ENUMERATE_ (Cell, icell, allCells()) {
      Cell cell = *icell;
      Real area = _computeAreaTriangle3(cell);
      for (Node node : cell.nodes()) {
        if (!(m_u_dirichlet[node]) && node.isOwn())
          // must replace rhs_values with numArray
          m_rhs_vect[node_dof.dofId(node, 0)] += f * area / ElementNodes;
      }
    }
    std::chrono::_V2::system_clock::time_point sassemby_stop;
    if (m_register_time) {
      sassemby_stop = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double> sassembly_duration = sassemby_stop - sassemby_start;
      sassembly_time = sassembly_duration.count();
      logger << "Constant source term assembly duration  for CSR: " << sassembly_time << "\n";
      fassemby_start = std::chrono::high_resolution_clock::now();
    }
  }
  {
    Timer::Action timer_action(this->subDomain(), "CsrConstantFluxTermAssembly");

    //----------------------------------------------
    // Constant flux term assembly
    //----------------------------------------------
    //
    //  only for noded that are non-Dirichlet
    //  $int_{dOmega_N}((q.n)*v^h)$
    // or
    //  $int_{dOmega_N}((n_x*q_x + n_y*q_y)*v^h)$
    //----------------------------------------------
    for (const auto& bs : options()->neumannBoundaryCondition()) {
      FaceGroup group = bs->surface();

      if (bs->value.isPresent()) {
        Real value = bs->value();
        ENUMERATE_ (Face, iface, group) {
          Face face = *iface;
          Real length = _computeEdgeLength2(face);
          for (Node node : iface->nodes()) {
            if (!(m_u_dirichlet[node]) && node.isOwn())
              // must replace rhs_values with numArray
              m_rhs_vect[node_dof.dofId(node, 0)] += value * length / 2.;
          }
        }
        continue;
      }

      if (bs->valueX.isPresent() && bs->valueY.isPresent()) {
        Real valueX = bs->valueX();
        Real valueY = bs->valueY();
        ENUMERATE_ (Face, iface, group) {
          Face face = *iface;
          Real length = _computeEdgeLength2(face);
          Real2 Normal = _computeEdgeNormal2(face);
          for (Node node : iface->nodes()) {
            if (!(m_u_dirichlet[node]) && node.isOwn())
              // must replace rhs_values with numArray
              m_rhs_vect[node_dof.dofId(node, 0)] += (Normal.x * valueX + Normal.y * valueY) * length / 2.;
          }
        }
        continue;
      }

      if (bs->valueX.isPresent()) {
        Real valueX = bs->valueX();
        ENUMERATE_ (Face, iface, group) {
          Face face = *iface;
          Real length = _computeEdgeLength2(face);
          Real2 Normal = _computeEdgeNormal2(face);
          for (Node node : iface->nodes()) {
            if (!(m_u_dirichlet[node]) && node.isOwn())
              // must replace rhs_values with numArray
              m_rhs_vect[node_dof.dofId(node, 0)] += (Normal.x * valueX) * length / 2.;
          }
        }
        continue;
      }

      if (bs->valueY.isPresent()) {
        Real valueY = bs->valueY();
        ENUMERATE_ (Face, iface, group) {
          Face face = *iface;
          Real length = _computeEdgeLength2(face);
          Real2 Normal = _computeEdgeNormal2(face);
          for (Node node : iface->nodes()) {
            if (!(m_u_dirichlet[node]) && node.isOwn())
              // must replace rhs_values with numArray
              m_rhs_vect[node_dof.dofId(node, 0)] += (Normal.y * valueY) * length / 2.;
          }
        }
        continue;
      }
    }
    std::chrono::_V2::system_clock::time_point fassemby_stop;
    if (m_register_time) {

      fassemby_stop = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double> fassembly_duration = fassemby_stop - fassemby_start;
      fassembly_time = fassembly_duration.count();
      logger << "Constant flux term assembly duration for CSR: " << fassembly_time << "\n";
      auto rhs_end = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double> duration = rhs_end - rhs_start;
      rhs_time = duration.count();
      logger << "RHS total duration for CSR: " << duration.count() << "\n";
      if (penalty_time != 0)
        logger << "Penalty time in rhs for CSR: " << penalty_time / rhs_time * 100 << "%\n";
      else
        logger << "Weak Penalty time in rhs for CSR: " << wpenalty_time / rhs_time * 100 << "%\n";
      logger << "Constant source term assembly time in rhs for CSR: " << sassembly_time / rhs_time * 100 << "%\n"
             << "Constant flux term assembly time in rhs for CSR: " << fassembly_time / rhs_time * 100 << "%\n\n"
             << "-------------------------------------------------------------------------------------\n\n";
    }
  }
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void FemModule::
_translateRhs()
{
  VariableDoFReal& rhs_values(m_linear_system.rhsVariable());
  rhs_values.fill(0.0);
  for (Int32 i = 0; i < m_rhs_vect.dim1Size(); i++) {

    rhs_values[DoFLocalId(i)] = m_rhs_vect[DoFLocalId(i)];
  }
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

Real FemModule::
_computeAreaQuad4(Cell cell)
{
  Real3 m0 = m_node_coord[cell.nodeId(0)];
  Real3 m1 = m_node_coord[cell.nodeId(1)];
  Real3 m2 = m_node_coord[cell.nodeId(2)];
  Real3 m3 = m_node_coord[cell.nodeId(3)];
  return 0.5 * ((m1.x * m2.y + m2.x * m3.y + m3.x * m0.y + m0.x * m1.y) - (m2.x * m1.y + m3.x * m2.y + m0.x * m3.y + m1.x * m0.y));
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

Real FemModule::
_computeAreaTriangle3(Cell cell)
{
  Real3 m0 = m_node_coord[cell.nodeId(0)];
  Real3 m1 = m_node_coord[cell.nodeId(1)];
  Real3 m2 = m_node_coord[cell.nodeId(2)];
  return 0.5 * ((m1.x - m0.x) * (m2.y - m0.y) - (m2.x - m0.x) * (m1.y - m0.y));
}

/*---------------------------------------------------------------------------*/
/*----------------------------#endif-----------------------------------------------*/

Real FemModule::
_computeEdgeLength2(Face face)
{
  Real3 m0 = m_node_coord[face.nodeId(0)];
  Real3 m1 = m_node_coord[face.nodeId(1)];
  return math::sqrt((m1.x - m0.x) * (m1.x - m0.x) + (m1.y - m0.y) * (m1.y - m0.y));
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

Real2 FemModule::
_computeEdgeNormal2(Face face)
{
  Real3 m0 = m_node_coord[face.nodeId(0)];
  Real3 m1 = m_node_coord[face.nodeId(1)];
  if (!face.isSubDomainBoundaryOutside())
    std::swap(m0, m1);
  Real2 N;
  Real norm_N = math::sqrt((m1.y - m0.y) * (m1.y - m0.y) + (m1.x - m0.x) * (m1.x - m0.x)); // for normalizing
  N.x = (m1.y - m0.y) / norm_N;
  N.y = (m0.x - m1.x) / norm_N;
  return N;
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

FixedMatrix<3, 3> FemModule::
_computeElementMatrixTRIA3(Cell cell)
{
  // Get coordiantes of the triangle element  TRI3
  //------------------------------------------------
  //                  0 o
  //                   . .
  //                  .   .
  //                 .     .
  //              1 o . . . o 2
  //------------------------------------------------
  Real3 m0 = m_node_coord[cell.nodeId(0)];
  Real3 m1 = m_node_coord[cell.nodeId(1)];
  Real3 m2 = m_node_coord[cell.nodeId(2)];

  Real area = _computeAreaTriangle3(cell); // calculate area

  Real2 dPhi0(m1.y - m2.y, m2.x - m1.x);
  Real2 dPhi1(m2.y - m0.y, m0.x - m2.x);
  Real2 dPhi2(m0.y - m1.y, m1.x - m0.x);

  FixedMatrix<2, 3> b_matrix;
  b_matrix(0, 0) = dPhi0.x;
  b_matrix(0, 1) = dPhi1.x;
  b_matrix(0, 2) = dPhi2.x;

  b_matrix(1, 0) = dPhi0.y;
  b_matrix(1, 1) = dPhi1.y;
  b_matrix(1, 2) = dPhi2.y;

  b_matrix.multInPlace(1.0 / (2.0 * area));

  FixedMatrix<3, 3> int_cdPi_dPj = matrixMultiplication(matrixTranspose(b_matrix), b_matrix);
  int_cdPi_dPj.multInPlace(area);

  //info() << "Cell=" << cell.localId();
  //std::cout << " int_cdPi_dPj=";
  //int_cdPi_dPj.dump(std::cout);
  //std::cout << "\n";

  return int_cdPi_dPj;
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

FixedMatrix<4, 4> FemModule::
_computeElementMatrixQUAD4(Cell cell)
{
  // Get coordiantes of the quadrangular element  QUAD4
  //------------------------------------------------
  //             1 o . . . . o 0
  //               .         .
  //               .         .
  //               .         .
  //             2 o . . . . o 3
  //------------------------------------------------
  Real3 m0 = m_node_coord[cell.nodeId(0)];
  Real3 m1 = m_node_coord[cell.nodeId(1)];
  Real3 m2 = m_node_coord[cell.nodeId(2)];
  Real3 m3 = m_node_coord[cell.nodeId(3)];

  Real area = _computeAreaQuad4(cell); // calculate area

  Real2 dPhi0(m2.y - m3.y, m3.x - m2.x);
  Real2 dPhi1(m3.y - m0.y, m0.x - m3.x);
  Real2 dPhi2(m0.y - m1.y, m1.x - m0.x);
  Real2 dPhi3(m1.y - m2.y, m2.x - m1.x);

  FixedMatrix<2, 4> b_matrix;
  b_matrix(0, 0) = dPhi0.x;
  b_matrix(0, 1) = dPhi1.x;
  b_matrix(0, 2) = dPhi2.x;
  b_matrix(0, 3) = dPhi3.x;

  b_matrix(1, 0) = dPhi0.y;
  b_matrix(1, 1) = dPhi1.y;
  b_matrix(1, 2) = dPhi2.y;
  b_matrix(1, 3) = dPhi3.y;

  b_matrix.multInPlace(1.0 / (2.0 * area));

  FixedMatrix<4, 4> int_cdPi_dPj = matrixMultiplication(matrixTranspose(b_matrix), b_matrix);
  int_cdPi_dPj.multInPlace(area);

  //info() << "Cell=" << cell.localId();
  //std::cout << " int_cdPi_dPj=";
  //int_cdPi_dPj.dump(std::cout);
  //std::cout << "\n";

  return int_cdPi_dPj;
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void FemModule::
_assembleBilinearOperatorQUAD4()
{
  auto node_dof(m_dofs_on_nodes.nodeDoFConnectivityView());

  ENUMERATE_ (Cell, icell, allCells()) {
    Cell cell = *icell;
    if (cell.type() != IT_Quad4)
      ARCANE_FATAL("Only Quad4 cell type is supported");

    auto K_e = _computeElementMatrixQUAD4(cell); // element stifness matrix
    //             # assemble elementary matrix into the global one
    //             # elementary terms are positionned into K according
    //             # to the rank of associated node in the mesh.nodes list
    //             for node1 in elem.nodes:
    //                 inode1=elem.nodes.index(node1) # get position of node1 in nodes list
    //                 for node2 in elem.nodes:
    //                     inode2=elem.nodes.index(node2)
    //                     K[node1.rank,node2.rank]=K[node1.rank,node2.rank]+K_e[inode1,inode2]
    Int32 n1_index = 0;
    for (Node node1 : cell.nodes()) {
      Int32 n2_index = 0;
      for (Node node2 : cell.nodes()) {
        // K[node1.rank,node2.rank]=K[node1.rank,node2.rank]+K_e[inode1,inode2]
        Real v = K_e(n1_index, n2_index);
        // m_k_matrix(node1.localId(), node2.localId()) += v;
        if (node1.isOwn()) {
          m_linear_system.matrixAddValue(node_dof.dofId(node1, 0), node_dof.dofId(node2, 0), v);
        }
        ++n2_index;
      }
      ++n1_index;
    }
  }
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

#ifdef ARCANE_HAS_CUDA

ARCCORE_HOST_DEVICE void FemModule::
_computeElementMatrixTRIA3GPU(CellLocalId icell, IndexedCellNodeConnectivityView cnc, ax::VariableNodeReal3InView in_node_coord, Real K_e[9])
{
  // Get coordiantes of the triangle element  TRI3
  //------------------------------------------------
  //                  0 o
  //                   . .
  //                  .   .
  //                 .     .
  //              1 o . . . o 2
  //------------------------------------------------
  Real3 m0 = in_node_coord[cnc.nodeId(icell, 0)];
  Real3 m1 = in_node_coord[cnc.nodeId(icell, 1)];
  Real3 m2 = in_node_coord[cnc.nodeId(icell, 2)];

  Real area = 0.5 * ((m1.x - m0.x) * (m2.y - m0.y) - (m2.x - m0.x) * (m1.y - m0.y)); // calculate area

  Real2 dPhi0(m1.y - m2.y, m2.x - m1.x);
  Real2 dPhi1(m2.y - m0.y, m0.x - m2.x);
  Real2 dPhi2(m0.y - m1.y, m1.x - m0.x);

  //We will want to replace fixed matrix by some numarray ? Will not work because NumArray function are host functions
  //NumArray<Real, ExtentsV<2, 3>> b_matrix(eMemoryRessource::Device);
  Real b_matrix[2][3] = { 0 };
  b_matrix[0][0] = dPhi0.x * (1.0 / (2.0 * area));
  b_matrix[0][1] = dPhi1.x * (1.0 / (2.0 * area));
  b_matrix[0][2] = dPhi2.x * (1.0 / (2.0 * area));

  b_matrix[1][0] = dPhi0.y * (1.0 / (2.0 * area));
  b_matrix[1][1] = dPhi1.y * (1.0 / (2.0 * area));
  b_matrix[1][2] = dPhi2.y * (1.0 / (2.0 * area));

  //NumArray<Real, ExtentsV<3, 3>> int_cdPi_dPj;

  //Multiplying b_matrix by its transpose, and doing the mult in place in the same loop
  for (Int32 i = 0; i < 3; i++) {
    for (Int32 j = 0; j < 3; j++) {
      Real x = 0.0;
      for (Int32 k = 0; k < 2; k++) {
        x += b_matrix[k][i] * b_matrix[k][j];
      }
      K_e[i * 3 + j] = x * area;
    }
  }

  //info() << "Cell=" << cell.localId();
  //std::cout << " int_cdPi_dPj=";
  //int_cdPi_dPj.dump(std::cout);
  //std::cout << "\n";

  //No need to return anymore
  //return int_cdPi_dPj;
}

#endif
#ifdef USE_COO_GPU
//Currently, this code does not work
/**
 * @brief Initialization of the coo matrix. It only works for p=1 since there is
 * one node per Edge. There is no difference with buildMatrix() and this method currently.
 * 
 * 
 */
void FemModule::
_buildMatrixGPU()
{
  //Initialization of the coo matrix;
  //This formula only works in p=1

  /*
  //Create a connection between nodes through the faces
  //Useless here because we only need this information once
  IItemFamily* node_family = mesh()->nodeFamily();
  NodeGroup nodes = node_family->allItems();
  auto idx_cn = mesh()->indexedConnectivityMng()->findOrCreateConnectivity(node_family, node_family, "NodeToNeighbourFaceNodes");
  auto* cn = idx_cn->connectivity();
  ENUMERATE_NODE (node, allNodes()) {
  }
  */

  Int32 nnz = nbFace() * 2 + nbNode();
  m_coo_matrix.initialize(m_dof_family, nnz);
  auto node_dof(m_dofs_on_nodes.nodeDoFConnectivityView());

  //We iterate through the node, and we do not sort anymore : we assume the nodes ID are sorted, and we will iterate throught the column to avoid making < and > comparison
  ENUMERATE_NODE (inode, allNodes()) {
    Node node = *inode;

    m_coo_matrix.setCoordinates(node_dof.dofId(node, 0), node_dof.dofId(node, 0));

    for (Face face : node.faces()) {
      if (face.nodeId(0) == node.localId())
        m_coo_matrix.setCoordinates(node_dof.dofId(node, 0), node_dof.dofId(face.nodeId(1), 0));
      else
        m_coo_matrix.setCoordinates(node_dof.dofId(node, 0), node_dof.dofId(face.nodeId(0), 0));
    }
  }

  //In this commented code, we begin by filling the diagonal before filling what's left by iterating through the nodes. It corresponds to the COO-sort method in the diagrams
  /*
  //Fill the diagonal
  ENUMERATE_NODE (inode, allNodes()) {
    Node node = *inode;
    m_coo_matrix.setCoordinates(node_dof.dofId(node, 0), node_dof.dofId(node, 0));
  }

  //Fill what is left
  ENUMERATE_FACE (iface, allFaces()) {
    Face face = *iface;
    auto nodes = face.nodes();
    for (Int32 i = 0; i < nodes.size() - i - 1; i++) {
      m_coo_matrix.setCoordinates(node_dof.dofId(nodes[i], 0), node_dof.dofId(nodes[nodes.size() - i - 1], 0));
      m_coo_matrix.setCoordinates(node_dof.dofId(nodes[nodes.size() - i - 1], 0), node_dof.dofId(nodes[i], 0));
    }
  }

  //Sort the matrix
  m_coo_matrix.sort();
  */
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void FemModule::
_assembleCooGPUBilinearOperatorTRIA3()
{
#if defined(m_register_time) && defined(m_cache_warming)
  logger << "-------------------------------------------------------------------------------------\n"
         << "Using CPU coo with NumArray format\n";
  auto lhs_start = std::chrono::high_resolution_clock::now();
  double compute_average = 0;
  double global_build_average = 0;
  double build_time = 0;
#endif
  // Build the coo matrix
  _buildMatrixGPU();
#if defined(m_register_time) && defined(m_cache_warming)
  auto build_stop = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> build_duration = build_stop - lhs_start;
  build_time = build_duration.count();
#endif

  RunQueue* queue = acceleratorMng()->defaultQueue();
  // Boucle sur les mailles déportée sur accélérateur
  auto command = makeCommand(queue);

  auto node_dof(m_dofs_on_nodes.nodeDoFConnectivityView());
  auto in_row_coo = ax::viewIn(command, m_coo_matrix.m_matrix_row);
  auto in_col_coo = ax::viewIn(command, m_coo_matrix.m_matrix_column);
  auto in_out_val_coo = ax::viewInOut(command, m_coo_matrix.m_matrix_value);
  UnstructuredMeshConnectivityView m_connectivity_view;
  auto in_node_coord = ax::viewIn(command, m_node_coord);
  m_connectivity_view.setMesh(this->mesh());
  auto cnc = m_connectivity_view.cellNode();
  Arcane::ItemGenericInfoListView nodes_infos(this->mesh()->nodeFamily());
  Arcane::ItemGenericInfoListView cells_infos(this->mesh()->cellFamily());

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
        Real v = K_e[n1_index * 3 + n2_index];
        // m_k_matrix(node1.localId(), node2.localId()) += v;
        //replacing the isOwn (probably with a nice view)
        if (nodes_infos.isOwn(node1)) {
          //m_coo_matrix.matrixAddValue(node_dof.dofId(node1, 0), node_dof.dofId(node2, 0), v);
        }
        ++n2_index;
      }
      ++n1_index;
    }
  };

#if defined(m_register_time) && defined(m_cache_warming)
  if (i == 3) {
    auto lhs_end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = lhs_end - lhs_start;
    double lhs_loc_time = duration.count();
    logger << "Building time of the coo matrix :" << build_time << "\n"
           << "Compute Elements average time : " << compute_average / nbCell() << "\n"
           << "Compute Elements total time : " << compute_average << "\n"
           << "Add in global matrix average time : " << global_build_average / nbCell() << "\n"
           << "Add in global matrix total time : " << global_build_average << "\n"
           << "LHS Total time : " << lhs_loc_time << "\n"
           << "Build matrix time in lhs :" << build_time / lhs_loc_time * 100 << "%\n"
           << "Compute element time in lhs : " << compute_average / lhs_loc_time * 100 << "%\n"
           << "Add in global matrix time in lhs : " << global_build_average / lhs_loc_time * 100 << "%\n\n"
           << "-------------------------------------------------------------------------------------\n\n";
    lhs_time += lhs_loc_time;
  }
#endif
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

#endif

void FemModule::
_solve()
{

  Timer::Action timer_action(this->subDomain(), "Solving");

  std::chrono::_V2::system_clock::time_point solve_start;
  if (m_register_time) {
    solve_start = std::chrono::high_resolution_clock::now();
  }

  m_linear_system.solve();

  // Re-Apply boundary conditions because the solver has modified the value
  // of u on all nodes
  _applyDirichletBoundaryConditions();

  {
    VariableDoFReal& dof_u(m_linear_system.solutionVariable());
    // Copy RHS DoF to Node u
    auto node_dof(m_dofs_on_nodes.nodeDoFConnectivityView());
    ENUMERATE_ (Node, inode, ownNodes()) {
      Node node = *inode;
      Real v = dof_u[node_dof.dofId(node, 0)];
      m_u[node] = v;
    }
  }

  //test
  m_u.synchronize();
  // def update_T(self,T):
  //     """Update u value on nodes after the FE resolution"""
  //     for i in range(0,len(self.mesh.nodes)):
  //         node=self.mesh.nodes[i]
  //         # don't update T imposed by Dirichlet BC
  //         if not node.is_T_fixed:
  //             self.mesh.nodes[i].T=T[i]

  const bool do_print = (allNodes().size() < 200);
  if (do_print) {
    ENUMERATE_ (Node, inode, allNodes()) {
      Node node = *inode;
      info() << "T[" << node.localId() << "][" << node.uniqueId() << "] = "
             << m_u[node];
      //info() << "T[]" << node.uniqueId() << " "
      //       << m_u[node];
    }
  }

  if (m_register_time) {
    auto solve_end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> solve_duration = solve_end - solve_start;
    solver_time = solve_duration.count();
    logger << "Solver duration : " << solver_time << "\n";
  }
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void FemModule::
_checkResultFile()
{
  String filename = options()->resultFile();
  info() << "CheckResultFile filename=" << filename;
  if (filename.empty())
    return;
  const double epsilon = 1.0e-4;
  checkNodeResultFile(traceMng(), filename, m_u, epsilon);
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

ARCANE_REGISTER_MODULE_FEM(FemModule);

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

#include "CsrBiliAssembly.hxx"
#include "CooBiliAssembly.hxx"
#include "CooSortBiliAssembly.hxx"
#include "LegacyBiliAssembly.hxx"

#ifdef ARCANE_HAS_CUDA
#include "CsrGpuBiliAssembly.hxx"
#endif

#ifdef USE_CUSPARSE_ADD
#include "CusparseBiliAssembly.hxx"
#endif

#ifdef ARCANE_HAS_CUDA
#include "NodeWiseCsrBiliAssembly.hxx"
#include "BlCsrBiliAssembly.hxx"
#endif
