/* ---------------------------------------------------------------------
 *
 * Copyright (C) 1999 - 2015 by the deal.II authors
 *
 * This file is part of the deal.II library.
 *
 * The deal.II library is free software; you can use it, redistribute
 * it, and/or modify it under the terms of the GNU Lesser General
 * Public License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * The full text of the license can be found in the file LICENSE at
 * the top level of the deal.II distribution.
 *
 * ---------------------------------------------------------------------

 *
 * Author: Wolfgang Bangerth, University of Heidelberg, 1999
 */



#include <deal.II/grid/tria.h>
#include <deal.II/dofs/dof_handler.h>
#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/tria_accessor.h>
#include <deal.II/grid/tria_iterator.h>
#include <deal.II/dofs/dof_accessor.h>
#include <deal.II/fe/fe_q.h>
#include <deal.II/dofs/dof_tools.h>
#include <deal.II/fe/fe_values.h>
#include <deal.II/base/quadrature_lib.h>
#include <deal.II/base/function.h>
#include <deal.II/numerics/vector_tools.h>
#include <deal.II/numerics/matrix_tools.h>
#include <deal.II/lac/vector.h>
#include <deal.II/lac/full_matrix.h>
#include <deal.II/lac/sparse_matrix.h>
#include <deal.II/lac/dynamic_sparsity_pattern.h>
#include <deal.II/lac/solver_cg.h>
#include <deal.II/lac/precondition.h>

#include <deal.II/numerics/data_out.h>
#include <fstream>
#include <iostream>

#include <deal.II/base/logstream.h>

#include <deal.II/numerics/data_postprocessor.h>
#include <deal.II/numerics/data_component_interpretation.h>


using namespace dealii;


template <int dim>
class Step4
{
public:
  Step4 ();
  void run ();

private:
  void make_grid ();
  void setup_system();
  void assemble_system ();
  void solve ();
  void output_results () const;

  Triangulation<dim>   triangulation;
  FE_Q<dim>            fe;
  DoFHandler<dim>      dof_handler;

  SparsityPattern      sparsity_pattern;
  SparseMatrix<double> system_matrix;

  Vector<double>       solution;
  Vector<double>       system_rhs;
};



template <int dim>
class RightHandSide : public Function<dim>
{
public:
  RightHandSide () : Function<dim>() {}

  virtual double value (const Point<dim>   &p,
                        const unsigned int  component = 0) const;
};



template <int dim>
class BoundaryValues : public Function<dim>
{
public:
  BoundaryValues () : Function<dim>() {}

  virtual double value (const Point<dim>   &p,
                        const unsigned int  component = 0) const;
};




template <int dim>
double RightHandSide<dim>::value (const Point<dim> &p,
                                  const unsigned int /*component*/) const
{
  double return_value = 1;

  return return_value;
}


template <int dim>
double BoundaryValues<dim>::value (const Point<dim> &p,
                                   const unsigned int /*component*/) const
{
  return (0.0);
}


template <int dim>
class Postprocessor : public DataPostprocessor<dim>
{
public:
  Postprocessor ();

  virtual void compute_derived_quantities_scalar (const std::vector<double>             &uh,
                                                  const std::vector<Tensor<1,dim> >     &duh,
                                                  const std::vector<Tensor<2,dim> >     &dduh,
                                                  const std::vector<Point<dim> >        &normals,
                                                  const std::vector<Point<dim> >        &points,
                                                  std::vector<Vector<double> >          &computed_quantities) const;

  virtual std::vector<std::string> get_names() const;

  virtual std::vector<DataComponentInterpretation::DataComponentInterpretation>
  get_data_component_interpretation() const;

  virtual UpdateFlags get_needed_update_flags() const;

private:
};




template <int dim>
Step4<dim>::Step4 ()
  :
  fe (2),
  dof_handler (triangulation)
{}



template <int dim>
void Step4<dim>::make_grid ()
{
  GridGenerator::hyper_cube (triangulation, -1, 1);
  triangulation.refine_global (4);

  std::cout << "   Number of active cells: "
            << triangulation.n_active_cells()
            << std::endl
            << "   Total number of cells: "
            << triangulation.n_cells()
            << std::endl;
}


template <int dim>
void Step4<dim>::setup_system ()
{
  dof_handler.distribute_dofs (fe);

  std::cout << "   Number of degrees of freedom: "
            << dof_handler.n_dofs()
            << std::endl;

  DynamicSparsityPattern dsp(dof_handler.n_dofs());
  DoFTools::make_sparsity_pattern (dof_handler, dsp);
  sparsity_pattern.copy_from(dsp);

  system_matrix.reinit (sparsity_pattern);

  solution.reinit (dof_handler.n_dofs());
  system_rhs.reinit (dof_handler.n_dofs());
}



template <int dim>
void Step4<dim>::assemble_system ()
{
  QGauss<dim>  quadrature_formula(2);

  const RightHandSide<dim> right_hand_side;

  FEValues<dim> fe_values (fe, quadrature_formula,
                           update_values   | update_gradients |
                           update_quadrature_points | update_JxW_values);

  const unsigned int   dofs_per_cell = fe.dofs_per_cell;
  const unsigned int   n_q_points    = quadrature_formula.size();

  FullMatrix<double>   cell_matrix (dofs_per_cell, dofs_per_cell);
  Vector<double>       cell_rhs (dofs_per_cell);

  std::vector<types::global_dof_index> local_dof_indices (dofs_per_cell);

  typename DoFHandler<dim>::active_cell_iterator
  cell = dof_handler.begin_active(),
  endc = dof_handler.end();

  for (; cell!=endc; ++cell)
    {
      fe_values.reinit (cell);
      cell_matrix = 0;
      cell_rhs = 0;

      for (unsigned int q_index=0; q_index<n_q_points; ++q_index)
        for (unsigned int i=0; i<dofs_per_cell; ++i)
          {
            for (unsigned int j=0; j<dofs_per_cell; ++j)
              cell_matrix(i,j) += (fe_values.shape_grad (i, q_index) *
                                   fe_values.shape_grad (j, q_index) *
                                   fe_values.JxW (q_index));

            cell_rhs(i) += (fe_values.shape_value (i, q_index) *
                            right_hand_side.value (fe_values.quadrature_point (q_index)) *
                            fe_values.JxW (q_index));
          }

      cell->get_dof_indices (local_dof_indices);
      for (unsigned int i=0; i<dofs_per_cell; ++i)
        {
          for (unsigned int j=0; j<dofs_per_cell; ++j)
            system_matrix.add (local_dof_indices[i],
                               local_dof_indices[j],
                               cell_matrix(i,j));

          system_rhs(local_dof_indices[i]) += cell_rhs(i);
        }
    }


  std::map<types::global_dof_index,double> boundary_values;
  VectorTools::interpolate_boundary_values (dof_handler,
                                            0,
                                            BoundaryValues<dim>(),
                                            boundary_values);
  MatrixTools::apply_boundary_values (boundary_values,
                                      system_matrix,
                                      solution,
                                      system_rhs);
}



template <int dim>
void Step4<dim>::solve ()
{
  SolverControl           solver_control (1000, 1e-12);
  SolverCG<>              solver (solver_control);

  PreconditionSSOR<> preconditioner;
  preconditioner.initialize(system_matrix, 1.0);

  solver.solve (system_matrix, solution, system_rhs,
                preconditioner);

  std::cout << "   " << solver_control.last_step()
            << " CG iterations needed to obtain convergence."
            << std::endl;
}



template <int dim>
void Step4<dim>::output_results () const
{
  Postprocessor<dim> postprocessor;
  DataOut<dim> data_out;
  data_out.attach_dof_handler (dof_handler);
  data_out.add_data_vector (solution, "solution");
  data_out.add_data_vector (solution, postprocessor);

  data_out.build_patches ();

  std::ofstream output (dim == 2 ?
                        "solution-2d.vtk" :
                        "solution-3d.vtk");
  data_out.write_vtk (output);
}




template <int dim>
void Step4<dim>::run ()
{
  std::cout << "Solving problem in " << dim << " space dimensions." << std::endl;

  make_grid();
  setup_system ();
  assemble_system ();
  solve ();
  output_results ();
}



// Post processor

template <int dim>
Postprocessor<dim>::
Postprocessor ()
{}

template <int dim>
void
Postprocessor<dim>::
compute_derived_quantities_scalar (const std::vector<double>             &uh,
                                   const std::vector<Tensor<1,dim> >     &duh,
                                   const std::vector<Tensor<2,dim> >     &/*dduh*/,
                                   const std::vector<Point<dim> >        &/*normals*/,
                                   const std::vector<Point<dim> >        &/*points*/,
                                   std::vector<Vector<double> >          &computed_quantities) const
{
  const unsigned int n_quadrature_points = static_cast<const unsigned int> (uh.size());

  Assert (duh.size() == n_quadrature_points, ExcInternalError());
  Assert (computed_quantities.size() == n_quadrature_points, ExcInternalError());
  Assert (computed_quantities[0].size() == dim + 2, ExcInternalError());

  for (unsigned int q=0; q<n_quadrature_points; ++q)
    {
      const double l2_square = duh[q] * duh[q];
      double l1(0.0);

      for (unsigned int d=0; d<dim; ++d)
      {
        computed_quantities[q][d] = duh[q][d];
        l1 += std::abs(duh[q][d]);
      }

      Assert (l2_square + 2.0*uh[q] >= 0.0, ExcMessage ("Sqrt of negative!"));
      // Min wall distance
      computed_quantities[q][dim]   = std::sqrt(std::max(l2_square + 2.0*uh[q], 0.0)) - l1;
      // Min wall distance
      computed_quantities[q][dim+1] = std::sqrt(std::max(l2_square + 2.0*uh[q], 0.0)) + l1;
    }
  return;
}


template <int dim>
std::vector<std::string>
Postprocessor<dim>::
get_names() const
{
  std::vector<std::string> names;
  for (unsigned int d=0; d<dim; ++d)
    {
      names.push_back ("Diretion");
    }
  names.push_back ("sMin");
  names.push_back ("sMax");

  return names;
}


template <int dim>
std::vector<DataComponentInterpretation::DataComponentInterpretation>
Postprocessor<dim>::
get_data_component_interpretation() const
{
  std::vector<DataComponentInterpretation::DataComponentInterpretation>
  interpretation (dim,
                  DataComponentInterpretation::component_is_part_of_vector);

  // Minimum wall distance
  interpretation.push_back (DataComponentInterpretation::
                            component_is_scalar);
  // Maximum wall distance
  interpretation.push_back (DataComponentInterpretation::
                            component_is_scalar);
  return interpretation;
}



template <int dim>
UpdateFlags
Postprocessor<dim>::
get_needed_update_flags() const
{
  return (update_values | update_gradients);
}



int main ()
{
  deallog.depth_console (0);
  {
    Step4<2> laplace_problem_2d;
    laplace_problem_2d.run ();
  }

  // {
  //   Step4<3> laplace_problem_3d;
  //   laplace_problem_3d.run ();
  // }

  return 0;
}
