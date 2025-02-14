﻿// -*- tab-width: 2; indent-tabs-mode: nil; coding: utf-8-with-signature -*-
//-----------------------------------------------------------------------------
// Copyright 2000-2023 CEA (www.cea.fr) IFPEN (www.ifpenergiesnouvelles.com)
// See the top-level COPYRIGHT file for details.
// SPDX-License-Identifier: Apache-2.0
//-----------------------------------------------------------------------------
/*---------------------------------------------------------------------------*/
/* NodeLinearSystem.h                                           (C) 2022-2023 */
/*                                                                           */
/* Linear system: Matrix A + Vector x + Vector b for Ax=b.                   */
/*---------------------------------------------------------------------------*/
#ifndef FEMTEST_FEMLINEARSYSTEM_H
#define FEMTEST_FEMLINEARSYSTEM_H
/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

#include <arcane/ItemTypes.h>
#include <arcane/VariableTypedef.h>

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

namespace Arcane::FemUtils
{

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/
/*!
 * \internal.
 * \brief Implementation for NodeLinearSystem.
 */
class NodeLinearSystemImpl
{
 public:

  virtual ~NodeLinearSystemImpl() = default;

 public:

  virtual void matrixAddValue(Arcane::NodeLocalId row, Arcane::NodeLocalId column, Arcane::Real value) = 0;
  virtual void setRHSValues(Arcane::Span<const Arcane::Real> values) = 0;
  virtual void solve() = 0;
};

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/
/*!
 * \brief Linear system Matrix A + Vector x + Vector b for Ax=b.
 *
 * Before using an instance of this class you need to call method
 * initialize(). If you want to reuse the same instance for several solving
 * you need to call reset() to destroy the underlying linear system and then
 * you need to call initialize() again.
 */
class NodeLinearSystem
{
 public:

  NodeLinearSystem();
  ~NodeLinearSystem();
  NodeLinearSystem(const NodeLinearSystem&) = delete;
  NodeLinearSystem(NodeLinearSystem&&) = delete;
  NodeLinearSystem& operator=(NodeLinearSystem&&) = delete;
  NodeLinearSystem& operator=(const NodeLinearSystem&) = delete;

 public:

  /*!
   * \brief Initialize the instance.
   *
   * The variable node_variable will be filled with the solution value after
   * the call to the method solve().
   */
  void initialize(Arcane::ISubDomain* sd, const Arcane::VariableNodeReal& node_variable);

  //! Add the value \a value to the (row,column) element of the matrix
  void matrixAddValue(Arcane::NodeLocalId row, Arcane::NodeLocalId column, Arcane::Real value);

  /*!
   * \brief Set the values for vector B.
   *
   * There is one value in values for each own nodes of the current sub-domain.
   */
  void setRHSValues(Arcane::Span<const Arcane::Real> values);

  /*!
   * \brief Solve the current linear system.
   */
  void solve();

  /*!
   * \brief Reset the current instance.
   *
   * You have to call initialize() again to re-use the same instance
   */
  void reset();

 private:

  NodeLinearSystemImpl* m_p = nullptr;

 private:

  void _checkInit();
};

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

#endif
