﻿// -*- tab-width: 2; indent-tabs-mode: nil; coding: utf-8-with-signature -*-
//-----------------------------------------------------------------------------
// Copyright 2000-2023 CEA (www.cea.fr) IFPEN (www.ifpenergiesnouvelles.com)
// See the top-level COPYRIGHT file for details.
// SPDX-License-Identifier: Apache-2.0
//-----------------------------------------------------------------------------
/*---------------------------------------------------------------------------*/
/* HypreDoFLinearSystem.cc                                     (C) 2022-2024 */
/*                                                                           */
/* Linear system: Matrix A + Vector x + Vector b for Ax=b.                   */
/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

#include "DoFLinearSystem.h"

#include <arcane/utils/FatalErrorException.h>
#include <arcane/utils/NumArray.h>
#include <arcane/utils/PlatformUtils.h>
#include <arcane/utils/ITraceMng.h>

#include <arcane/core/VariableTypes.h>
#include <arcane/core/IItemFamily.h>
#include <arcane/core/BasicService.h>
#include <arcane/core/ServiceFactory.h>
#include <arcane/core/IParallelMng.h>
#include <arcane/core/ItemPrinter.h>

#include <arcane/accelerator/core/Runner.h>

#include "FemUtils.h"
#include "IDoFLinearSystemFactory.h"

#include "HypreDoFLinearSystemFactory_axl.h"

#include <HYPRE.h>
#include <HYPRE_parcsr_ls.h>
#include <krylov.h>

// NOTE:
// DoF family must be compacted (i.e maxLocalId()==nbItem()) and sorted
// for this implementation to works.

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

namespace Arcane::FemUtils
{
using namespace Arcane;
namespace
{
  inline void
  check(const char* hypre_func, int error_code)
  {
    if (error_code == 0)
      return;
    char buf[8192];
    HYPRE_DescribeError(error_code, buf);
    cout << "\nXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
         << "\nHYPRE ERROR in function "
         << hypre_func
         << "\nError_code=" << error_code
         << "\nMessage=" << buf
         << "\nXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
         << "\n"
         << std::flush;
    throw Exception("HYPRE Check", hypre_func);
  }
  inline void
  hypreCheck(const char* hypre_func, int error_code)
  {
    check(hypre_func, error_code);
    int r = HYPRE_GetError();
    if (r != 0)
      cout << "HYPRE GET ERROR r=" << r
           << " error_code=" << error_code << " func=" << hypre_func << '\n';
  }
} // namespace

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

class HypreDoFLinearSystemImpl
: public TraceAccessor
, public DoFLinearSystemImpl
{
 public:

  HypreDoFLinearSystemImpl(IItemFamily* dof_family, const String& solver_name)
  : TraceAccessor(dof_family->traceMng())
  , m_dof_family(dof_family)
  , m_rhs_variable(VariableBuildInfo(dof_family, solver_name + "RHSVariable"))
  , m_dof_variable(VariableBuildInfo(dof_family, solver_name + "SolutionVariable"))
  , m_dof_matrix_indexes(VariableBuildInfo(m_dof_family, solver_name + "DoFMatrixIndexes"))
  , m_dof_elimination_info(VariableBuildInfo(m_dof_family, solver_name + "DoFEliminationInfo"))
  , m_dof_elimination_value(VariableBuildInfo(m_dof_family, solver_name + "DoFEliminationValue"))
  , m_dof_matrix_numbering(VariableBuildInfo(dof_family, solver_name + "MatrixNumbering"))
  {
    info() << "Creating HypreDoFLinearSystemImpl()";
  }

  ~HypreDoFLinearSystemImpl()
  {
    info() << "Calling HYPRE_Finalize";
    HYPRE_Finalize(); /* must be the last HYPRE function call */
  }

 public:

  void build()
  {
    HYPRE_Init(); /* must be the first HYPRE function call */
  }

 public:

  void matrixAddValue(DoFLocalId row, DoFLocalId column, Real value) override
  {
    ARCANE_THROW(NotImplementedException, "");
  }

  void matrixSetValue(DoFLocalId row, DoFLocalId column, Real value) override
  {
    ARCANE_THROW(NotImplementedException, "");
  }

  void eliminateRow(DoFLocalId row, Real value) override
  {
    ARCANE_THROW(NotImplementedException, "");
  }

  void eliminateRowColumn(DoFLocalId row, Real value) override
  {
    ARCANE_THROW(NotImplementedException, "");
  }

  void solve() override;

  VariableDoFReal& solutionVariable() override
  {
    return m_dof_variable;
  }

  VariableDoFReal& rhsVariable() override
  {
    return m_rhs_variable;
  }

  void setSolverCommandLineArguments(const CommandLineArguments& args) override
  {
  }

  void clearValues()
  {
    info() << "Clear values";
    m_csr_view = {};
  }

  void setCSRValues(const CSRFormatView& csr_view) override
  {
    m_csr_view = csr_view;
  }
  bool hasSetCSRValues() const override { return true; }

  void setRunner(Runner* r) override { m_runner = r; }
  Runner* runner() const { return m_runner; }

 private:

  IItemFamily* m_dof_family = nullptr;
  VariableDoFReal m_rhs_variable;
  VariableDoFReal m_dof_variable;
  VariableDoFInt32 m_dof_matrix_indexes;
  VariableDoFByte m_dof_elimination_info;
  VariableDoFReal m_dof_elimination_value;
  VariableDoFInt32 m_dof_matrix_numbering;
  NumArray<Int32, MDDim1> m_parallel_columns_index;
  NumArray<Int32, MDDim1> m_parallel_rows_index;
  //! Work array to store values of solution vector in parallel
  NumArray<Real, MDDim1> m_result_work_values;
  Runner* m_runner = nullptr;

  CSRFormatView m_csr_view;
  Int32 m_first_own_row = -1;
  Int32 m_nb_own_row = -1;

 private:

  void _computeMatrixNumerotation();
};

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void HypreDoFLinearSystemImpl::
_computeMatrixNumerotation()
{
  IParallelMng* pm = m_dof_family->parallelMng();
  const bool is_parallel = pm->isParallel();
  const Int32 nb_rank = pm->commSize();
  const Int32 my_rank = pm->commRank();

  DoFGroup all_dofs = m_dof_family->allItems();
  DoFGroup own_dofs = all_dofs.own();
  const Int32 nb_own_row = own_dofs.size();

  Int32 own_first_index = 0;

  if (is_parallel) {
    // TODO: utiliser un Scan lorsque ce sera disponible dans Arcane
    UniqueArray<Int32> parallel_rows_index(nb_rank, 0);
    pm->allGather(ConstArrayView<Int32>(1, &nb_own_row), parallel_rows_index);
    info() << "ALL_NB_ROW = " << parallel_rows_index;
    for (Int32 i = 0; i < my_rank; ++i)
      own_first_index += parallel_rows_index[i];
  }

  info() << "OwnFirstIndex=" << own_first_index << " NbOwnRow=" << nb_own_row;

  m_first_own_row = own_first_index;
  m_nb_own_row = nb_own_row;

  // TODO: Faire avec API accelerateur
  ENUMERATE_DOF (idof, own_dofs) {
    DoF dof = *idof;
    m_dof_matrix_numbering[idof] = own_first_index + idof.index();
    //info() << "Numbering dof_uid=" << dof.uniqueId() << " M=" << m_dof_matrix_numbering[idof];
  }
  info() << " nb_own_row=" << nb_own_row << " nb_item=" << m_dof_family->nbItem();
  m_dof_matrix_numbering.synchronize();

  m_parallel_rows_index.resize(nb_own_row);
  m_result_work_values.resize(nb_own_row);
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void HypreDoFLinearSystemImpl::
solve()
{
  HYPRE_MemoryLocation hypre_memory = HYPRE_MEMORY_HOST;
  HYPRE_ExecutionPolicy hypre_exec_policy = HYPRE_EXEC_HOST;

  // Récupère le communicateur MPI associé
  IParallelMng* pm = m_dof_family->parallelMng();
  Parallel::Communicator arcane_comm = pm->communicator();
  MPI_Comm mpi_comm = MPI_COMM_WORLD;
  if (arcane_comm.isValid())
    mpi_comm = static_cast<MPI_Comm>(arcane_comm);

  bool is_parallel = pm->isParallel();
  const Int32 nb_rank = pm->commSize();
  const Int32 my_rank = pm->commRank();

  // TODO: A ne faire qu'un fois sauf si les DoFs évoluent
  _computeMatrixNumerotation();

  bool is_use_device = false;
  if (m_runner) {
    is_use_device = isAcceleratorPolicy(m_runner->executionPolicy());
    info() << "Runner for Hypre=" << m_runner->executionPolicy() << " is_device=" << is_use_device;
  }

  // Si HYPRE n'est pas compilé avec le support GPU, alors on utilise l'hôte.
  // (NOTE: a priori il n'y a pas besoin de faire cela. Si Hypre n'est pas compilé avec
  // support GPU alors HYPRE_MEMORY_DEVICE <=> HYPRE_MEMORY_HOST
  // TODO: détecter la cohérence entre le GPU de Hypre et le notre (.i.e les deux
  // utilisent CUDA ou ROCM)
#ifndef HYPRE_USING_GPU
  if (is_use_device)
    info() << "Hypre is not compiled with GPU support. Using host backend";
#endif

  if (is_use_device) {
    m_runner->setAsCurrentDevice();
    hypre_memory = HYPRE_MEMORY_DEVICE;
    hypre_exec_policy = HYPRE_EXEC_DEVICE;
  }

  hypreCheck("HYPRE_SetMemoryLocation", HYPRE_SetMemoryLocation(hypre_memory));
  /* setup AMG on GPUs */
  hypreCheck("HYPRE_SetExecutionPolicy", HYPRE_SetExecutionPolicy(hypre_exec_policy));

  if (is_use_device) {
#if HYPRE_RELEASE_NUMBER >= 22300
    /* use hypre's SpGEMM instead of vendor implementation */
    HYPRE_SetSpGemmUseVendor(false);
#endif
    /* use GPU RNG */
    HYPRE_SetUseGpuRand(true);
  }

  /* use hypre's GPU memory pool */
  //HYPRE_SetGPUMemoryPoolSize(bin_growth, min_bin, max_bin, max_bytes);

  /* setup IJ matrix A */

  HYPRE_IJMatrix ij_A = nullptr;
  HYPRE_ParCSRMatrix parcsr_A = nullptr;

  const bool do_debug_print = false;
  const bool do_dump_matrix = false;

  Span<const Int32> rows_index_span = m_dof_matrix_numbering.asArray();
  const Int32 nb_local_row = rows_index_span.size();

  if (do_debug_print) {
    info() << "ROWS_INDEX=" << rows_index_span;
    info() << "ROWS=" << m_csr_view.rows();
    info() << "ROWS_NB_COLUMNS=" << m_csr_view.rowsNbColumn();
    info() << "COLUMNS=" << m_csr_view.columns();
    info() << "VALUE=" << m_csr_view.values();
  }

  const int first_row = m_first_own_row;
  const int last_row = m_first_own_row + m_nb_own_row - 1;

  info() << "CreateMatrix first_row=" << first_row << " last_row " << last_row;
  HYPRE_IJMatrixCreate(mpi_comm, first_row, last_row, first_row, last_row, &ij_A);

  int* rows_nb_column_data = const_cast<int*>(m_csr_view.rowsNbColumn().data());

  Real m1 = platform::getRealTime();
  HYPRE_IJMatrixSetObjectType(ij_A, HYPRE_PARCSR);
  HYPRE_IJMatrixInitialize_v2(ij_A, hypre_memory);

  // m_csr_view.columns() use matrix coordinates local to sub-domain
  // We need to translate them to global matrix coordinates
  Span<const Int32> columns_index_span = m_csr_view.columns();
  if (is_parallel) {
    // TODO: Faire sur accélérateur et ne faire qu'une fois si la structure
    // ne change pas.
    Int64 nb_column = columns_index_span.size();
    m_parallel_columns_index.resize(nb_column);
    for (Int64 i = 0; i < nb_column; ++i) {
      DoFLocalId lid(columns_index_span[i]);
      //info() << "I=" << i << " index=" << columns_index_span[i];
      // Si lid correspond à une entité nulle, alors la valeur de la matrice
      // ne sera pas utilisée.
      if (!lid.isNull())
        m_parallel_columns_index[i] = m_dof_matrix_numbering[lid];
      else
        m_parallel_columns_index[i] = 0;
    }
    columns_index_span = m_parallel_columns_index.to1DSpan();
  }

  if (do_debug_print) {
    info() << "FINAL_COLUMNS=" << columns_index_span;
    info() << "NbValue=" << m_csr_view.values().size();
  }

  Span<const Real> matrix_values = m_csr_view.values();
  if (do_debug_print) {
    ENUMERATE_ (DoF, idof, m_dof_family->allItems()) {
      DoF dof = *idof;
      Int32 nb_col = m_csr_view.rowsNbColumn()[idof.index()];
      Int32 row_csr_index = m_csr_view.rows()[idof.index()];
      info() << "DoF dof=" << ItemPrinter(dof) << " nb_col=" << nb_col << " row_csr_index=" << row_csr_index
             << " global_row=" << rows_index_span[idof.index()];
      for (Int32 i = 0; i < nb_col; ++i) {
        Int32 col_index = m_csr_view.columns()[row_csr_index + i];
        if (col_index >= 0)
          info() << "COL=" << col_index
                 << " T_COL=" << m_dof_matrix_numbering[DoFLocalId(col_index)]
                 << " V=" << matrix_values[row_csr_index + i];
        else
          info() << "COL=" << col_index
                 << " X_COL=" << columns_index_span[row_csr_index + i]
                 << " V=" << matrix_values[row_csr_index + i];
      }
    }
  }

  if (is_parallel) {
    // Fill 'm_parallel_rows_index' with only rows we owns
    // NOTE: This is only needed if matrix structure has changed.
    Int32 index = 0;
    ENUMERATE_ (DoF, idof, m_dof_family->allItems()) {
      DoF dof = *idof;
      if (!dof.isOwn())
        continue;
      Int32 nb_col = m_csr_view.rowsNbColumn()[idof.index()];
      m_parallel_rows_index[index] = rows_index_span[idof.index()];
      ++index;
    }
  }

  /* GPU pointers; efficient in large chunks */
  HYPRE_IJMatrixSetValues(ij_A,
                          nb_local_row,
                          rows_nb_column_data,
                          rows_index_span.data(),
                          columns_index_span.data(),
                          matrix_values.data());

  HYPRE_IJMatrixAssemble(ij_A);
  HYPRE_IJMatrixGetObject(ij_A, (void**)&parcsr_A);
  Real m2 = platform::getRealTime();
  info() << "Time to create matrix=" << (m2 - m1);

  if (do_dump_matrix) {
    String file_name = String("dumpA.") + String::fromNumber(my_rank) + ".txt";
    HYPRE_IJMatrixPrint(ij_A, file_name.localstr());
    pm->traceMng()->flush();
    pm->barrier();
  }

  HYPRE_IJVector ij_vector_b = nullptr;
  HYPRE_ParVector parvector_b = nullptr;
  HYPRE_IJVector ij_vector_x = nullptr;
  HYPRE_ParVector parvector_x = nullptr;

  hypreCheck("IJVectorCreate", HYPRE_IJVectorCreate(mpi_comm, first_row, last_row, &ij_vector_b));
  hypreCheck("IJVectorSetObjectType", HYPRE_IJVectorSetObjectType(ij_vector_b, HYPRE_PARCSR));
  HYPRE_IJVectorInitialize_v2(ij_vector_b, hypre_memory);

  hypreCheck("IJVectorCreate", HYPRE_IJVectorCreate(mpi_comm, first_row, last_row, &ij_vector_x));
  hypreCheck("IJVectorSetObjectType", HYPRE_IJVectorSetObjectType(ij_vector_x, HYPRE_PARCSR));
  HYPRE_IJVectorInitialize_v2(ij_vector_x, hypre_memory);

  Real v1 = platform::getRealTime();
  hypreCheck("HYPRE_IJVectorSetValues",
             HYPRE_IJVectorSetValues(ij_vector_b, nb_local_row, rows_index_span.data(),
                                     m_rhs_variable.asArray().data()));

  hypreCheck("HYPRE_IJVectorSetValues",
             HYPRE_IJVectorSetValues(ij_vector_x, nb_local_row, rows_index_span.data(),
                                     m_dof_variable.asArray().data()));

  hypreCheck("HYPRE_IJVectorAssemble",
             HYPRE_IJVectorAssemble(ij_vector_b));
  HYPRE_IJVectorGetObject(ij_vector_b, (void**)&parvector_b);

  hypreCheck("HYPRE_IJVectorAssemble",
             HYPRE_IJVectorAssemble(ij_vector_x));
  HYPRE_IJVectorGetObject(ij_vector_x, (void**)&parvector_x);
  Real v2 = platform::getRealTime();
  info() << "Time to create vectors=" << (v2 - v1);

  if (do_dump_matrix) {
    String file_name_b = String("dumpB.") + String::fromNumber(my_rank) + ".txt";
    HYPRE_IJVectorPrint(ij_vector_b, file_name_b.localstr());
    String file_name_x = String("dumpX.") + String::fromNumber(my_rank) + ".txt";
    HYPRE_IJVectorPrint(ij_vector_x, file_name_x.localstr());
    pm->traceMng()->flush();
    pm->barrier();
  }

  HYPRE_Solver solver = nullptr;
  HYPRE_Solver precond = nullptr;
  /* setup AMG */
  HYPRE_ParCSRPCGCreate(mpi_comm, &solver);

  /* Set some parameters (See Reference Manual for more parameters) */
  HYPRE_PCGSetMaxIter(solver, 1000); /* max iterations */
  HYPRE_PCGSetTol(solver, 1e-7); /* conv. tolerance */
  HYPRE_PCGSetTwoNorm(solver, 1); /* use the two norm as the stopping criteria */
  HYPRE_PCGSetPrintLevel(solver, 2); /* print solve info */
  HYPRE_PCGSetLogging(solver, 1); /* needed to get run info later */

  hypreCheck("HYPRE_BoomerAMGCreate", HYPRE_BoomerAMGCreate(&precond));

  HYPRE_BoomerAMGCreate(&precond);
  HYPRE_BoomerAMGSetPrintLevel(precond, 1); /* print amg solution info */
  HYPRE_BoomerAMGSetCoarsenType(precond, 6);
  HYPRE_BoomerAMGSetOldDefault(precond);
  HYPRE_BoomerAMGSetRelaxType(precond, 6); /* Sym G.S./Jacobi hybrid */
  HYPRE_BoomerAMGSetNumSweeps(precond, 1);
  HYPRE_BoomerAMGSetTol(precond, 0.0); /* conv. tolerance zero */
  HYPRE_BoomerAMGSetMaxIter(precond, 1); /* do only one iteration! */

  hypreCheck("HYPRE_ParCSRPCGSetPrecond",
             HYPRE_ParCSRPCGSetPrecond(solver, HYPRE_BoomerAMGSolve, HYPRE_BoomerAMGSetup, precond));
  hypreCheck("HYPRE_PCGSetup",
             HYPRE_ParCSRPCGSetup(solver, parcsr_A, parvector_b, parvector_x));
  Real a1 = platform::getRealTime();
  hypreCheck("HYPRE_PCGSolve",
             HYPRE_ParCSRPCGSolve(solver, parcsr_A, parvector_b, parvector_x));
  Real b1 = platform::getRealTime();
  info() << "Time to solve=" << (b1 - a1);

  if (is_parallel) {
    Int32 nb_wanted_row = m_parallel_rows_index.extent0();
    hypreCheck("HYPRE_IJVectorGetValues",
               HYPRE_IJVectorGetValues(ij_vector_x, nb_wanted_row,
                                       m_parallel_rows_index.to1DSpan().data(),
                                       m_result_work_values.to1DSpan().data()));
    ENUMERATE_ (DoF, idof, m_dof_family->allItems().own()) {
      m_dof_variable[idof] = m_result_work_values[idof.index()];
    }
  }
  else {
    hypreCheck("HYPRE_IJVectorGetValues",
               HYPRE_IJVectorGetValues(ij_vector_x, nb_local_row, rows_index_span.data(),
                                       m_dof_variable.asArray().data()));
  }
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

class HypreDoFLinearSystemFactoryService
: public ArcaneHypreDoFLinearSystemFactoryObject
{
 public:

  explicit HypreDoFLinearSystemFactoryService(const ServiceBuildInfo& sbi)
  : ArcaneHypreDoFLinearSystemFactoryObject(sbi)
  {
    info() << "Create HypreDoF";
  }

  DoFLinearSystemImpl*
  createInstance(ISubDomain* sd, IItemFamily* dof_family, const String& solver_name) override
  {
    auto* x = new HypreDoFLinearSystemImpl(dof_family, solver_name);
    x->build();
    return x;
  }
};

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

ARCANE_REGISTER_SERVICE_HYPREDOFLINEARSYSTEMFACTORY(HypreLinearSystem,
                                                    HypreDoFLinearSystemFactoryService);

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

} // namespace Arcane::FemUtils

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/
