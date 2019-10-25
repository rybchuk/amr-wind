#include <NodalProjection.H>
#include <AMReX_Vector.H>
#include <AMReX_MultiFab.H>
#include <AMReX_MLNodeLaplacian.H>
#include <AMReX_MLMG.H>
#include <AMReX_ParmParse.H>

using namespace amrex;

NodalProjection::NodalProjection ( const amrex::Vector<amrex::Geometry>&             a_geom,
                                   const amrex::Vector<amrex::BoxArray>&             a_grids,
                                   const amrex::Vector<amrex::DistributionMapping>&  a_dmap,
                                   std::array<amrex::LinOpBCType,AMREX_SPACEDIM>     a_bc_lo,
                                   std::array<amrex::LinOpBCType,AMREX_SPACEDIM>     a_bc_hi,
                                   amrex::Vector<amrex::EBFArrayBoxFactory const *>  a_ebfactory )
{
    define(a_geom, a_grids, a_dmap, a_bc_lo, a_bc_hi, a_ebfactory);
}


void
NodalProjection::define ( const  amrex::Vector<amrex::Geometry>&                    a_geom,
                          const  amrex::Vector<amrex::BoxArray>&                    a_grids,
                          const  amrex::Vector<amrex::DistributionMapping>&         a_dmap,
                          std::array<amrex::LinOpBCType,AMREX_SPACEDIM>             a_bc_lo,
                          std::array<amrex::LinOpBCType,AMREX_SPACEDIM>             a_bc_hi,
                          amrex::Vector<amrex::EBFArrayBoxFactory const *>          a_ebfactory )
{

    m_geom      = a_geom;
    m_grids     = a_grids;
    m_dmap      = a_dmap;
    m_bc_lo     = a_bc_lo;
    m_bc_hi     = a_bc_hi;
    m_ebfactory = a_ebfactory;

    int nlev( m_grids.size() );

    // Resize member data
    m_phi.resize(nlev);
    m_fluxes.resize(nlev);
    m_rhs.resize(nlev);

    // Allocate member data
    int ng(1);      // We use 1 ghost node only -- it should be enough

    for (int lev(0); lev < nlev; ++lev )
    {
        // Cell-centered data
        m_fluxes[lev].reset(new MultiFab(m_grids[lev], m_dmap[lev], 3, ng, MFInfo(), *m_ebfactory[lev]));

        // Node-centered data
        const auto& ba_nd = amrex::convert(m_grids[lev], IntVect{1,1,1});
        m_phi[lev].reset(new MultiFab(ba_nd, m_dmap[lev], 1, ng, MFInfo(), *m_ebfactory[lev]));
        m_rhs[lev].reset(new MultiFab(ba_nd, m_dmap[lev], 1, ng, MFInfo(), *m_ebfactory[lev]));
    }

    // Get inputs from ParmParse
    readParameters();

    // object is ready
    m_ok = true;

    // First setup
    setup();
}


//
// Perform projection:
//
//     vel = vel - sigma*grad(phi)
//
//  where phi is the solution of
//
//   div( sigma * grad(phi) ) = div(vel) + S_cc + S_nd
//
//  where vel, sigma, S_cc are cell-centered variables
//  and phi and S_nd are nodal variables
//
//  grad(phi) is node-centered.
//
void
NodalProjection::project (      Vector< std::unique_ptr< amrex::MultiFab > >& a_vel,
                          const Vector< std::unique_ptr< amrex::MultiFab > >& a_sigma,
                          const Vector< std::unique_ptr< amrex::MultiFab > >& a_S_cc,
                          const Vector< std::unique_ptr< amrex::MultiFab > >& a_S_nd )

{
    AMREX_ALWAYS_ASSERT(m_ok);
    BL_PROFILE("NodalProjection::project");

    amrex::Print() << "Nodal Projection:" << std::endl;

    // Setup solver -- ALWAYS do this because matrix may change
    setup();

    // Compute RHS
    m_matrix -> compRHS( GetVecOfPtrs(m_rhs),  GetVecOfPtrs(a_vel), GetVecOfConstPtrs(a_S_cc),
                         GetVecOfPtrs(a_S_nd) );

    // Print diagnostics
    amrex::Print() << " >> Before projection:" << std::endl;
    printInfo();

    // Set matrix coefficients
    for (int lev(0); lev < a_sigma.size(); ++lev)
        m_matrix -> setSigma(lev, *a_sigma[lev]);

    // Solve
    m_solver -> solve( GetVecOfPtrs(m_phi), GetVecOfConstPtrs(m_rhs), m_mg_rtol, m_mg_atol );

    // Get fluxes -- fluxes = - sigma*grad(phi)
    m_solver -> getFluxes( GetVecOfPtrs(m_fluxes) );

    // Perform projection
    for (int lev(0); lev < m_phi.size(); ++lev)
    {
        // vel = vel + fluxes = vel - sigma * grad(phi)
        MultiFab::Add( *a_vel[lev], *m_fluxes[lev], 0, 0, AMREX_SPACEDIM, 0);

        // set m_fluxes = -fluxes/sigma = grad(phi)
        m_fluxes[lev] -> mult(- 1.0, m_fluxes[lev]->nGrow() );
        for (int n(0); n < AMREX_SPACEDIM; ++n)
            MultiFab::Divide(*m_fluxes[lev], *a_sigma[lev], 0, n, 1, m_fluxes[lev]->nGrow() );

        // Fill boundaries and apply scale factor to phi
        m_phi[lev] -> FillBoundary( m_geom[lev].periodicity());

    }

    // Compute RHS -- this is only needed to print out post projection values
    m_matrix -> compRHS( GetVecOfPtrs(m_rhs),  GetVecOfPtrs(a_vel), GetVecOfConstPtrs(a_S_cc),
                         GetVecOfPtrs(a_S_nd) );

    // Print diagnostics
    amrex::Print() << " >> After projection:" << std::endl;
    printInfo();
}


//
// Read from input file
//
void
NodalProjection::readParameters ()
{
    ParmParse pp("projection");
    pp.query( "verbose"                , m_verbose );
    pp.query( "mg_verbose"             , m_mg_verbose );
    pp.query( "mg_cg_verbose"          , m_mg_cg_verbose );
    pp.query( "mg_maxiter"             , m_mg_maxiter );
    pp.query( "mg_cg_maxiter"          , m_mg_cg_maxiter );
    pp.query( "mg_rtol"                , m_mg_rtol );
    pp.query( "mg_atol"                , m_mg_atol );
    pp.query( "mg_max_coarsening_level", m_mg_max_coarsening_level );
    pp.query( "bottom_solver_type"     , m_bottom_solver_type );
}



//
// Setup object before solve
//
void
NodalProjection::setup ()
{
    BL_PROFILE("NodalProjection::setup");
    AMREX_ALWAYS_ASSERT(m_ok);

    // Initialize all variables
    for (int lev(0); lev < m_phi.size(); ++lev)
    {
        m_phi[lev] -> setVal(0.0);
        m_fluxes[lev] -> setVal(0.0);
        m_rhs[lev] ->  setVal(0.0);
    }

    //
    // Setup Matrix
    //
    LPInfo                       info;
    info.setMaxCoarseningLevel(m_mg_max_coarsening_level);
    m_matrix.reset(new MLNodeLaplacian(m_geom, m_grids, m_dmap, info, m_ebfactory));

    m_matrix->setGaussSeidel(true);
    m_matrix->setHarmonicAverage(false);
    m_matrix->setDomainBC(m_bc_lo, m_bc_hi);

    //
    // Setup solver
    //
    m_solver.reset(new MLMG(*m_matrix));

    m_solver->setMaxIter(m_mg_maxiter);
    m_solver->setVerbose(m_mg_verbose);
    m_solver->setCGVerbose(m_mg_cg_verbose);
    m_solver->setCGMaxIter(m_mg_cg_maxiter);

    if (m_bottom_solver_type == "smoother")
    {
        m_solver->setBottomSolver(MLMG::BottomSolver::smoother);
    }
    else if (m_bottom_solver_type == "bicg")
    {
        m_solver->setBottomSolver(MLMG::BottomSolver::bicgstab);
    }
    else if (m_bottom_solver_type == "cg")
    {
        m_solver->setBottomSolver(MLMG::BottomSolver::cg);
    }
    else if (m_bottom_solver_type == "bicgcg")
    {
        m_solver->setBottomSolver(MLMG::BottomSolver::bicgcg);
    }
    else if (m_bottom_solver_type == "cgbicg")
    {
        m_solver->setBottomSolver(MLMG::BottomSolver::cgbicg);
    }
    else if (m_bottom_solver_type == "hypre")
    {
#ifdef AMREX_USE_HYPRE
        m_solver->setBottomSolver(MLMG::BottomSolver::hypre);
#else
        amrex::Abort("AMReX was not built with HYPRE support");
#endif
    }

}


//
// Return DivU for diagnostics
//
void
NodalProjection::getDivU (Vector< std::unique_ptr< amrex::MultiFab > >& divu,
                          Vector< std::unique_ptr< amrex::MultiFab > >& a_vel,
                          Real a_time )
{
    AMREX_ALWAYS_ASSERT(m_ok);
    m_matrix -> compDivergence( GetVecOfPtrs(divu),  GetVecOfPtrs(a_vel));
}

//
// Compute RHS: div(u) (later this may have a specified S as in div(u) = S)
//

void
NodalProjection::computeRHS ( Vector< std::unique_ptr< amrex::MultiFab > >& a_vel )
{
    AMREX_ALWAYS_ASSERT(m_ok);
    BL_PROFILE("NodalProjection::computeRHS");

    // Compute div(eu)
    m_matrix -> compRHS( GetVecOfPtrs(m_rhs),  GetVecOfPtrs(a_vel), {}, {} );
}


void
NodalProjection::printInfo ()
{
    for (int lev(0); lev < m_rhs.size(); ++lev)
    {
        amrex::Print() << "  * On lev " << lev
                       << " max(abs(divu)) = "
                       << m_rhs[lev]->norm0(0,0,false,true)
                       << std::endl;
    }
}
