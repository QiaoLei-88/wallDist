/* ---------------------------------------------------------------------
 *
 * Copyright (C) 2009 - 2015 by the deal.II authors
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
 * Author: Wolfgang Bangerth, Texas A&M University, 2009, 2010
 *         Timo Heister, University of Goettingen, 2009, 2010
 */


#include <deal.II/base/quadrature_lib.h>
#include <deal.II/base/function.h>
#include <deal.II/base/timer.h>

#include <deal.II/lac/generic_linear_algebra.h>

#define USE_PETSC_LA

namespace LA
{
#ifdef USE_PETSC_LA
  using namespace dealii::LinearAlgebraPETSc;
#else
  using namespace dealii::LinearAlgebraTrilinos;
#endif
}

#include <deal.II/lac/vector.h>
#include <deal.II/lac/full_matrix.h>
#include <deal.II/lac/solver_cg.h>
#include <deal.II/lac/constraint_matrix.h>
#include <deal.II/lac/dynamic_sparsity_pattern.h>

#include <deal.II/lac/petsc_parallel_sparse_matrix.h>
#include <deal.II/lac/petsc_parallel_vector.h>
#include <deal.II/lac/petsc_solver.h>
#include <deal.II/lac/petsc_precondition.h>

#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/tria_accessor.h>
#include <deal.II/grid/tria_iterator.h>
#include <deal.II/dofs/dof_handler.h>
#include <deal.II/dofs/dof_accessor.h>
#include <deal.II/dofs/dof_tools.h>
#include <deal.II/fe/fe_values.h>
#include <deal.II/fe/fe_q.h>
#include <deal.II/numerics/vector_tools.h>
#include <deal.II/numerics/data_out.h>
#include <deal.II/numerics/error_estimator.h>

#include <deal.II/base/utilities.h>
#include <deal.II/base/conditional_ostream.h>
#include <deal.II/base/index_set.h>
#include <deal.II/lac/sparsity_tools.h>
#include <deal.II/distributed/tria.h>
#include <deal.II/distributed/grid_refinement.h>

#include <fstream>
#include <iostream>

namespace Step40
{
  using namespace dealii;


  template <int dim>
  class LaplaceProblem
  {
  public:
    LaplaceProblem ();
    ~LaplaceProblem ();

    void run ();

  private:
    void setup_system ();
    void assemble_system ();
    void solve ();
    void refine_grid ();
    void output_results (const unsigned int cycle) const;

    MPI_Comm                                  mpi_communicator;

    parallel::distributed::Triangulation<dim> triangulation;

    DoFHandler<dim>                           dof_handler;
    FE_Q<dim>                                 fe;

    IndexSet                                  locally_owned_dofs;
    IndexSet                                  locally_relevant_dofs;

    ConstraintMatrix                          constraints;

    LA::MPI::SparseMatrix                     system_matrix;
    LA::MPI::Vector                           locally_relevant_solution;
    LA::MPI::Vector                           system_rhs;

    ConditionalOStream                        pcout;
    TimerOutput                               computing_timer;
  };



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
  LaplaceProblem<dim>::LaplaceProblem ()
    :
    mpi_communicator (MPI_COMM_WORLD),
    triangulation (mpi_communicator,
                   typename Triangulation<dim>::MeshSmoothing
                   (Triangulation<dim>::smoothing_on_refinement |
                    Triangulation<dim>::smoothing_on_coarsening)),
    dof_handler (triangulation),
    fe (2),
    pcout (std::cout,
           (Utilities::MPI::this_mpi_process(mpi_communicator)
            == 0)),
    computing_timer (mpi_communicator,
                     pcout,
                     TimerOutput::summary,
                     TimerOutput::wall_times)
  {}



  template <int dim>
  LaplaceProblem<dim>::~LaplaceProblem ()
  {
    dof_handler.clear ();
  }



  template <int dim>
  void LaplaceProblem<dim>::setup_system ()
  {
    TimerOutput::Scope t(computing_timer, "setup");

    dof_handler.distribute_dofs (fe);

    locally_owned_dofs = dof_handler.locally_owned_dofs ();
    DoFTools::extract_locally_relevant_dofs (dof_handler,
                                             locally_relevant_dofs);

    locally_relevant_solution.reinit (locally_owned_dofs,
                                      locally_relevant_dofs, mpi_communicator);
    system_rhs.reinit (locally_owned_dofs, mpi_communicator);

    constraints.clear ();
    constraints.reinit (locally_relevant_dofs);
    DoFTools::make_hanging_node_constraints (dof_handler, constraints);
    VectorTools::interpolate_boundary_values (dof_handler,
                                              0,
                                              ZeroFunction<dim>(),
                                              constraints);
    constraints.close ();

    DynamicSparsityPattern dsp (locally_relevant_dofs);

    DoFTools::make_sparsity_pattern (dof_handler, dsp,
                                     constraints, false);
    SparsityTools::distribute_sparsity_pattern (dsp,
                                                dof_handler.n_locally_owned_dofs_per_processor(),
                                                mpi_communicator,
                                                locally_relevant_dofs);

    system_matrix.reinit (locally_owned_dofs,
                          locally_owned_dofs,
                          dsp,
                          mpi_communicator);
  }




  template <int dim>
  void LaplaceProblem<dim>::assemble_system ()
  {
    TimerOutput::Scope t(computing_timer, "assembly");

    const QGauss<dim>  quadrature_formula(3);

    FEValues<dim> fe_values (fe, quadrature_formula,
                             update_values    |  update_gradients |
                             update_quadrature_points |
                             update_JxW_values);

    const unsigned int   dofs_per_cell = fe.dofs_per_cell;
    const unsigned int   n_q_points    = quadrature_formula.size();

    FullMatrix<double>   cell_matrix (dofs_per_cell, dofs_per_cell);
    Vector<double>       cell_rhs (dofs_per_cell);

    std::vector<types::global_dof_index> local_dof_indices (dofs_per_cell);

    typename DoFHandler<dim>::active_cell_iterator
    cell = dof_handler.begin_active(),
    endc = dof_handler.end();
    for (; cell!=endc; ++cell)
      if (cell->is_locally_owned())
        {
          cell_matrix = 0;
          cell_rhs = 0;

          fe_values.reinit (cell);

          for (unsigned int q_point=0; q_point<n_q_points; ++q_point)
            {
              const double
              rhs_value = 1;
              for (unsigned int i=0; i<dofs_per_cell; ++i)
                {
                  for (unsigned int j=0; j<dofs_per_cell; ++j)
                    cell_matrix(i,j) += (fe_values.shape_grad(i,q_point) *
                                         fe_values.shape_grad(j,q_point) *
                                         fe_values.JxW(q_point));

                  cell_rhs(i) += (rhs_value *
                                  fe_values.shape_value(i,q_point) *
                                  fe_values.JxW(q_point));
                }
            }

          cell->get_dof_indices (local_dof_indices);
          constraints.distribute_local_to_global (cell_matrix,
                                                  cell_rhs,
                                                  local_dof_indices,
                                                  system_matrix,
                                                  system_rhs);
        }

    system_matrix.compress (VectorOperation::add);
    system_rhs.compress (VectorOperation::add);
  }




  template <int dim>
  void LaplaceProblem<dim>::solve ()
  {
    TimerOutput::Scope t(computing_timer, "solve");
    LA::MPI::Vector
    completely_distributed_solution (locally_owned_dofs, mpi_communicator);

    SolverControl solver_control (dof_handler.n_dofs(), 1e-12);

    LA::SolverCG solver(solver_control, mpi_communicator);
    LA::MPI::PreconditionAMG preconditioner;

    LA::MPI::PreconditionAMG::AdditionalData data;

#ifdef USE_PETSC_LA
    data.symmetric_operator = true;
#else
    /* Trilinos defaults are good */
#endif
    preconditioner.initialize(system_matrix, data);

    solver.solve (system_matrix, completely_distributed_solution, system_rhs,
                  preconditioner);

    pcout << "   Solved in " << solver_control.last_step()
          << " iterations." << std::endl;

    constraints.distribute (completely_distributed_solution);

    locally_relevant_solution = completely_distributed_solution;
  }




  template <int dim>
  void LaplaceProblem<dim>::refine_grid ()
  {
    TimerOutput::Scope t(computing_timer, "refine");

    Vector<float> estimated_error_per_cell (triangulation.n_active_cells());
    KellyErrorEstimator<dim>::estimate (dof_handler,
                                        QGauss<dim-1>(3),
                                        typename FunctionMap<dim>::type(),
                                        locally_relevant_solution,
                                        estimated_error_per_cell);
    parallel::distributed::GridRefinement::
    refine_and_coarsen_fixed_number (triangulation,
                                     estimated_error_per_cell,
                                     0.3, 0.03);
    triangulation.execute_coarsening_and_refinement ();
  }




  template <int dim>
  void LaplaceProblem<dim>::output_results (const unsigned int cycle) const
  {
    Postprocessor<dim> postprocessor;
    DataOut<dim> data_out;
    data_out.attach_dof_handler (dof_handler);
    data_out.add_data_vector (locally_relevant_solution, "u");

    Vector<float> subdomain (triangulation.n_active_cells());
    for (unsigned int i=0; i<subdomain.size(); ++i)
      subdomain(i) = triangulation.locally_owned_subdomain();
    data_out.add_data_vector (subdomain, "subdomain");

    data_out.add_data_vector (locally_relevant_solution, postprocessor);

    data_out.build_patches ();

    const std::string filename = ("solution-" +
                                  Utilities::int_to_string (cycle, 2) +
                                  "." +
                                  Utilities::int_to_string
                                  (triangulation.locally_owned_subdomain(), 4));
    std::ofstream output ((filename + ".vtu").c_str());
    data_out.write_vtu (output);

    if (Utilities::MPI::this_mpi_process(mpi_communicator) == 0)
      {
        std::vector<std::string> filenames;
        for (unsigned int i=0;
             i<Utilities::MPI::n_mpi_processes(mpi_communicator);
             ++i)
          filenames.push_back ("solution-" +
                               Utilities::int_to_string (cycle, 2) +
                               "." +
                               Utilities::int_to_string (i, 4) +
                               ".vtu");

        std::ofstream master_output (("solution-" +
                                      Utilities::int_to_string (cycle, 2) +
                                      ".pvtu").c_str());
        data_out.write_pvtu_record (master_output, filenames);
      }
  }




  template <int dim>
  void LaplaceProblem<dim>::run ()
  {
    const unsigned int n_cycles = 1;
    for (unsigned int cycle=0; cycle<n_cycles; ++cycle)
      {
        pcout << "Cycle " << cycle << ':' << std::endl;

        if (cycle == 0)
          {
            GridGenerator::hyper_cube (triangulation);
            for (typename Triangulation<dim>::active_cell_iterator
                 cell = triangulation.begin_active();
                 cell != triangulation.end();
                 ++cell)
              for (unsigned int f=0; f<GeometryInfo<dim>::faces_per_cell; ++f)
                if (cell->face (f)->at_boundary())
                  {
                    if (cell->face (f)-> center()[0] > 0.9 || cell->face (f)-> center()[1] > 0.9 )
                      {
                        cell->face (f)->set_boundary_id (5);
                      }
                  }

            triangulation.refine_global (5);
          }
        else
          refine_grid ();

        setup_system ();

        pcout << "   Number of active cells:       "
              << triangulation.n_global_active_cells()
              << std::endl
              << "   Number of degrees of freedom: "
              << dof_handler.n_dofs()
              << std::endl;

        assemble_system ();
        solve ();

        if (Utilities::MPI::n_mpi_processes(mpi_communicator) <= 32)
          {
            TimerOutput::Scope t(computing_timer, "output");
            output_results (cycle);
          }

        computing_timer.print_summary ();
        computing_timer.reset ();

        pcout << std::endl;
      }
  }



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
        computed_quantities[q][dim]   = std::sqrt(std::max(l2_square + 2.0*uh[q], 0.0)) - l1;
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

    interpretation.push_back (DataComponentInterpretation::
                              component_is_scalar);
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

}


int main(int argc, char *argv[])
{
  try
    {
      using namespace dealii;
      using namespace Step40;

      Utilities::MPI::MPI_InitFinalize mpi_initialization(argc, argv, 1);
      deallog.depth_console (0);

      {
        LaplaceProblem<2> laplace_problem_2d;
        laplace_problem_2d.run ();
      }
    }
  catch (std::exception &exc)
    {
      std::cerr << std::endl << std::endl
                << "----------------------------------------------------"
                << std::endl;
      std::cerr << "Exception on processing: " << std::endl
                << exc.what() << std::endl
                << "Aborting!" << std::endl
                << "----------------------------------------------------"
                << std::endl;

      return 1;
    }
  catch (...)
    {
      std::cerr << std::endl << std::endl
                << "----------------------------------------------------"
                << std::endl;
      std::cerr << "Unknown exception!" << std::endl
                << "Aborting!" << std::endl
                << "----------------------------------------------------"
                << std::endl;
      return 1;
    }

  return 0;
}
