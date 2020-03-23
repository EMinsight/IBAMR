// ---------------------------------------------------------------------
//
// Copyright (c) 2018 - 2019 by the IBAMR developers
// All rights reserved.
//
// This file is part of IBAMR.
//
// IBAMR is free software and is distributed under the 3-clause BSD
// license. The full text of the license can be found in the file
// COPYRIGHT at the top level directory of IBAMR.
//
// --------------------------------------------------------------------

/////////////////////////////// INCLUDES /////////////////////////////////////

#include "IBAMR_config.h"

#include "ibamr/AdvDiffCUIConservativeConvectiveOperator.h"
#include "ibamr/AdvDiffPhysicalBoundaryUtilities.h"
#include "ibamr/ConvectiveOperator.h"
#include "ibamr/ibamr_enums.h"
#include "ibamr/ibamr_utilities.h"
#include "ibamr/namespaces.h" // IWYU pragma: keep

#include "ibtk/CartExtrapPhysBdryOp.h"

#include "Box.h"
#include "CartesianGridGeometry.h"
#include "CartesianPatchGeometry.h"
#include "CellData.h"
#include "CellDataFactory.h"
#include "CellVariable.h"
#include "CoarsenAlgorithm.h"
#include "CoarsenOperator.h"
#include "CoarsenSchedule.h"
#include "FaceData.h"
#include "FaceVariable.h"
#include "Index.h"
#include "IntVector.h"
#include "MultiblockDataTranslator.h"
#include "Patch.h"
#include "PatchHierarchy.h"
#include "PatchLevel.h"
#include "RefineAlgorithm.h"
#include "RefineOperator.h"
#include "RefinePatchStrategy.h"
#include "RefineSchedule.h"
#include "RobinBcCoefStrategy.h"
#include "SAMRAIVectorReal.h"
#include "Variable.h"
#include "VariableContext.h"
#include "VariableDatabase.h"
#include "tbox/Database.h"
#include "tbox/Pointer.h"
#include "tbox/Timer.h"
#include "tbox/TimerManager.h"
#include "tbox/Utilities.h"

#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace SAMRAI
{
namespace solv
{
template <int DIM>
class RobinBcCoefStrategy;
} // namespace solv
} // namespace SAMRAI

// FORTRAN ROUTINES
#if (NDIM == 2)
#define ADVECT_FLUX_FC IBAMR_FC_FUNC_(advect_flux2d, ADVECT_FLUX2D)
#define CUI_EXTRAPOLATE_FC IBAMR_FC_FUNC_(cui_extrapolate2d, CUI_EXTRAPOLATE2D)
#define F_TO_C_DIV_FC IBAMR_FC_FUNC_(ftocdiv2d, FTOCDIV2D)
#endif

#if (NDIM == 3)
#define ADVECT_FLUX_FC IBAMR_FC_FUNC_(advect_flux3d, ADVECT_FLUX3D)
#define CUI_EXTRAPOLATE_FC IBAMR_FC_FUNC_(cui_extrapolate3d, CUI_EXTRAPOLATE3D)
#define F_TO_C_DIV_FC IBAMR_FC_FUNC_(ftocdiv3d, FTOCDIV3D)
#endif
extern "C"
{
    void CUI_EXTRAPOLATE_FC(
#if (NDIM == 2)
        const int&,
        const int&,
        const int&,
        const int&,
        const int&,
        const int&,
        const double*,
        double*,
        const int&,
        const int&,
        const int&,
        const int&,
        const double*,
        const double*,
        double*,
        double*
#endif
#if (NDIM == 3)
        const int&,
        const int&,
        const int&,
        const int&,
        const int&,
        const int&,
        const int&,
        const int&,
        const int&,
        const double*,
        double*,
        double*,
        const int&,
        const int&,
        const int&,
        const int&,
        const int&,
        const int&,
        const double*,
        const double*,
        const double*,
        double*,
        double*,
        double*
#endif
    );

    void ADVECT_FLUX_FC(const double&,
#if (NDIM == 2)
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const double*,
                        const double*,
                        const double*,
                        const double*,
                        double*,
                        double*
#endif
#if (NDIM == 3)
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const double*,
                        const double*,
                        const double*,
                        const double*,
                        const double*,
                        const double*,
                        double*,
                        double*,
                        double*
#endif
    );

    void F_TO_C_DIV_FC(double*,
                       const int&,
                       const double&,
#if (NDIM == 2)
                       const double*,
                       const double*,
                       const int&,
                       const int&,
                       const int&,
                       const int&,
                       const int&,
#endif
#if (NDIM == 3)
                       const double*,
                       const double*,
                       const double*,
                       const int&,
                       const int&,
                       const int&,
                       const int&,
                       const int&,
                       const int&,
                       const int&,
#endif
                       const double*);
}

/////////////////////////////// NAMESPACE ////////////////////////////////////

namespace IBAMR
{
/////////////////////////////// STATIC ///////////////////////////////////////

namespace
{
// NOTE: The number of ghost cells required by the advection scheme
// These values were chosen to work with CUI (the cubic interpolation
// upwind method of Waterson and Deconinck).
static const int GADVECTG = 2;

// Timers.
static Timer* t_apply_convective_operator;
static Timer* t_apply;
static Timer* t_initialize_operator_state;
static Timer* t_deallocate_operator_state;
} // namespace

/////////////////////////////// PUBLIC ///////////////////////////////////////

AdvDiffCUIConservativeConvectiveOperator::AdvDiffCUIConservativeConvectiveOperator(
    std::string object_name,
    Pointer<CellVariable<NDIM, double> > Q_var,
    Pointer<Database> input_db,
    const ConvectiveDifferencingType difference_form,
    std::vector<RobinBcCoefStrategy<NDIM>*> bc_coefs)
    : AdvDiffCUIConvectiveOperator(std::move(object_name), Q_var, input_db, difference_form, bc_coefs)
{
    VariableDatabase<NDIM>* var_db = VariableDatabase<NDIM>::getDatabase();
    Pointer<VariableContext> context = var_db->getContext(d_object_name + "::CONTEXT");
    d_rho_scratch_idx = var_db->registerVariableAndContext(d_rho_var, context, GADVECTG);
    Pointer<CellDataFactory<NDIM, double> > rho_pdat_fac = d_rho_var->getPatchDataFactory();
    d_rho_data_depth = rho_pdat_fac->getDefaultDepth();

    // registering extrapolated rho variable
    const std::string rho_extrap_var_name = d_object_name + "::rho_extrap";
    d_rho_extrap_var = var_db->getVariable(rho_extrap_var_name);
    if (d_rho_extrap_var)
    {
        d_rho_extrap_idx = var_db->mapVariableAndContextToIndex(d_rho_extrap_var, context);
    }
    else
    {
        d_rho_extrap_var = new FaceVariable<NDIM, double>(rho_extrap_var_name, d_rho_data_depth);
        d_rho_extrap_idx = var_db->registerVariableAndContext(d_rho_extrap_var, context, IntVector<NDIM>(0));
    }
#if !defined(NDEBUG)
    TBOX_ASSERT(d_rho_extrap_idx >= 0);
#endif
    const std::string rho_flux_var_name = d_object_name + "::rho_flux";
    d_rho_flux_var = var_db->getVariable(rho_flux_var_name);
    if (d_rho_flux_var)
    {
        d_rho_flux_idx = var_db->mapVariableAndContextToIndex(d_rho_flux_var, context);
    }
    else
    {
        d_rho_flux_var = new FaceVariable<NDIM, double>(rho_flux_var_name, d_rho_data_depth);
        d_rho_flux_idx = var_db->registerVariableAndContext(d_rho_flux_var, context, IntVector<NDIM>(0));
    }
#if !defined(NDEBUG)
    TBOX_ASSERT(d_rho_flux_idx >= 0);
#endif

    // Setup Timers.
    IBAMR_DO_ONCE(
        t_apply_convective_operator =
            TimerManager::getManager()->getTimer("IBAMR::AdvDiffCUIConvectiveOperator::applyConvectiveOperator()");
        t_apply = TimerManager::getManager()->getTimer("IBAMR::AdvDiffCUIConvectiveOperator::apply()");
        t_initialize_operator_state =
            TimerManager::getManager()->getTimer("IBAMR::AdvDiffCUIConvectiveOperator::initializeOperatorState()");
        t_deallocate_operator_state =
            TimerManager::getManager()->getTimer("IBAMR::AdvDiffCUIConvectiveOperator::deallocateOperatorState()"););
    return;

} // AdvDiffCUIConservativeConvectiveOperator

AdvDiffCUIConservativeConvectiveOperator::~AdvDiffCUIConservativeConvectiveOperator()
{
    deallocateOperatorState();
    return;
} // ~AdvDiffCUIConservativeConvectiveOperator

void
AdvDiffCUIConservativeConvectiveOperator::applyConvectiveOperator(const int Q_idx, const int N_idx)
{
    IBAMR_TIMER_START(t_apply_convective_operator);

#if !defined(NDEBUG)
    if (!d_is_initialized)
    {
        TBOX_ERROR("AdvDiffCUIConvectiveOperator::applyConvectiveOperator():\n"
                   << "  operator must be initialized prior to call to applyConvectiveOperator\n");
    }
#endif
    // Allocate scratch data.
    for (int ln = d_coarsest_ln; ln <= d_finest_ln; ++ln)
    {
        Pointer<PatchLevel<NDIM> > level = d_hierarchy->getPatchLevel(ln);
        level->allocatePatchData(d_Q_scratch_idx);
        level->allocatePatchData(d_q_extrap_idx);
        level->allocatePatchData(d_q_flux_idx);

        // for density
        level->allocatePatchData(d_rho_scratch_idx);
        level->allocatePatchData(d_rho_extrap_idx);
        level->allocatePatchData(d_rho_flux_idx);
    }

    // Setup communications algorithm.
    Pointer<CartesianGridGeometry<NDIM> > grid_geom = d_hierarchy->getGridGeometry();
    Pointer<RefineAlgorithm<NDIM> > refine_alg = new RefineAlgorithm<NDIM>();
    Pointer<RefineOperator<NDIM> > refine_op = grid_geom->lookupRefineOperator(d_Q_var, "CONSERVATIVE_LINEAR_REFINE");
    refine_alg->registerRefine(d_Q_scratch_idx, Q_idx, d_Q_scratch_idx, refine_op);
    // for density
    Pointer<RefineOperator<NDIM> > refine_op_rho =
        grid_geom->lookupRefineOperator(d_rho_var, "CONSERVATIVE_LINEAR_REFINE");
    refine_alg->registerRefine(d_rho_scratch_idx, d_rho_idx, d_rho_scratch_idx, refine_op_rho);

    // Extrapolate from cell centers to cell faces.
    for (int ln = d_coarsest_ln; ln <= d_finest_ln; ++ln)
    {
        refine_alg->resetSchedule(d_ghostfill_scheds[ln]);
        d_ghostfill_scheds[ln]->fillData(d_solution_time);
        d_ghostfill_alg->resetSchedule(d_ghostfill_scheds[ln]);
        Pointer<PatchLevel<NDIM> > level = d_hierarchy->getPatchLevel(ln);
        for (PatchLevel<NDIM>::Iterator p(level); p; p++)
        {
            Pointer<Patch<NDIM> > patch = level->getPatch(p());

            const Box<NDIM>& patch_box = patch->getBox();
            const IntVector<NDIM>& patch_lower = patch_box.lower();
            const IntVector<NDIM>& patch_upper = patch_box.upper();

            Pointer<CellData<NDIM, double> > Q_data = patch->getPatchData(d_Q_scratch_idx);
            const IntVector<NDIM>& Q_data_gcw = Q_data->getGhostCellWidth();
#if !defined(NDEBUG)
            TBOX_ASSERT(Q_data_gcw.min() == Q_data_gcw.max());
#endif
            Pointer<FaceData<NDIM, double> > u_ADV_data = patch->getPatchData(d_u_idx);
            const IntVector<NDIM>& u_ADV_data_gcw = u_ADV_data->getGhostCellWidth();
#if !defined(NDEBUG)
            TBOX_ASSERT(u_ADV_data_gcw.min() == u_ADV_data_gcw.max());
#endif
            Pointer<FaceData<NDIM, double> > q_extrap_data = patch->getPatchData(d_q_extrap_idx);
            const IntVector<NDIM>& q_extrap_data_gcw = q_extrap_data->getGhostCellWidth();
#if !defined(NDEBUG)
            TBOX_ASSERT(q_extrap_data_gcw.min() == q_extrap_data_gcw.max());
#endif
            CellData<NDIM, double>& Q0_data = *Q_data;
            CellData<NDIM, double> Q1_data(patch_box, 1, Q_data_gcw);
#if (NDIM == 3)
            CellData<NDIM, double> Q2_data(patch_box, 1, Q_data_gcw);
#endif

            // for density
            Pointer<CellData<NDIM, double> > rho_data = patch->getPatchData(d_rho_scratch_idx);
            const IntVector<NDIM>& rho_data_gcw = rho_data->getGhostCellWidth();
#if !defined(NDEBUG)
            TBOX_ASSERT(rho_data_gcw.min() == rho_data_gcw.max());
#endif
            Pointer<FaceData<NDIM, double> > rho_extrap_data = patch->getPatchData(d_rho_extrap_idx);
            const IntVector<NDIM>& rho_extrap_data_gcw = rho_extrap_data->getGhostCellWidth();
#if !defined(NDEBUG)
            TBOX_ASSERT(rho_extrap_data_gcw.min() == rho_extrap_data_gcw.max());
#endif
            CellData<NDIM, double>& rho0_data = *rho_data;
            CellData<NDIM, double> rho1_data(patch_box, 1, rho_data_gcw);
#if (NDIM == 3)
            CellData<NDIM, double> rho2_data(patch_box, 1, rho_data_gcw);
#endif
            // Enforce physical boundary conditions at inflow boundaries.
            AdvDiffPhysicalBoundaryUtilities::setPhysicalBoundaryConditions(
                Q_data,
                u_ADV_data,
                patch,
                d_bc_coefs,
                d_solution_time,
                /*inflow_boundary_only*/ d_outflow_bdry_extrap_type != "NONE",
                d_homogeneous_bc);

            // Enforce physical boundary conditions at inflow boundaries.
            AdvDiffPhysicalBoundaryUtilities::setPhysicalBoundaryConditions(
                rho_data,
                u_ADV_data,
                patch,
                d_rho_bc_coefs,
                d_solution_time,
                /*inflow_boundary_only*/ d_outflow_bdry_extrap_type != "NONE",
                d_homogeneous_bc);

            // Extrapolate the Q_var from cell centers to cell faces
            for (unsigned int d = 0; d < d_Q_data_depth; ++d)
            {
                CUI_EXTRAPOLATE_FC(
#if (NDIM == 2)
                    patch_lower(0),
                    patch_upper(0),
                    patch_lower(1),
                    patch_upper(1),
                    Q_data_gcw(0),
                    Q_data_gcw(1),
                    Q0_data.getPointer(d),
                    Q1_data.getPointer(),
                    u_ADV_data_gcw(0),
                    u_ADV_data_gcw(1),
                    q_extrap_data_gcw(0),
                    q_extrap_data_gcw(1),
                    u_ADV_data->getPointer(0),
                    u_ADV_data->getPointer(1),
                    q_extrap_data->getPointer(0, d),
                    q_extrap_data->getPointer(1, d)
#endif
#if (NDIM == 3)
                        patch_lower(0),
                    patch_upper(0),
                    patch_lower(1),
                    patch_upper(1),
                    patch_lower(2),
                    patch_upper(2),
                    Q_data_gcw(0),
                    Q_data_gcw(1),
                    Q_data_gcw(2),
                    Q0_data.getPointer(d),
                    Q1_data.getPointer(),
                    Q2_data.getPointer(),
                    u_ADV_data_gcw(0),
                    u_ADV_data_gcw(1),
                    u_ADV_data_gcw(2),
                    q_extrap_data_gcw(0),
                    q_extrap_data_gcw(1),
                    q_extrap_data_gcw(2),
                    u_ADV_data->getPointer(0),
                    u_ADV_data->getPointer(1),
                    u_ADV_data->getPointer(2),
                    q_extrap_data->getPointer(0, d),
                    q_extrap_data->getPointer(1, d),
                    q_extrap_data->getPointer(2, d)
#endif
                );

                // Extrapolate the rho_var from cell centers to cell faces
                CUI_EXTRAPOLATE_FC(
#if (NDIM == 2)
                    patch_lower(0),
                    patch_upper(0),
                    patch_lower(1),
                    patch_upper(1),
                    rho_data_gcw(0),
                    rho_data_gcw(1),
                    rho0_data.getPointer(d),
                    rho1_data.getPointer(),
                    u_ADV_data_gcw(0),
                    u_ADV_data_gcw(1),
                    rho_extrap_data_gcw(0),
                    rho_extrap_data_gcw(1),
                    u_ADV_data->getPointer(0),
                    u_ADV_data->getPointer(1),
                    rho_extrap_data->getPointer(0, d),
                    rho_extrap_data->getPointer(1, d)
#endif
#if (NDIM == 3)
                        patch_lower(0),
                    patch_upper(0),
                    patch_lower(1),
                    patch_upper(1),
                    patch_lower(2),
                    patch_upper(2),
                    rho_data_gcw(0),
                    rho_data_gcw(1),
                    rho_data_gcw(2),
                    rho0_data.getPointer(d),
                    rho1_data.getPointer(),
                    rho2_data.getPointer(),
                    u_ADV_data_gcw(0),
                    u_ADV_data_gcw(1),
                    u_ADV_data_gcw(2),
                    rho_extrap_data_gcw(0),
                    rho_extrap_data_gcw(1),
                    rho_extrap_data_gcw(2),
                    u_ADV_data->getPointer(0),
                    u_ADV_data->getPointer(1),
                    u_ADV_data->getPointer(2),
                    rho_extrap_data->getPointer(0, d),
                    rho_extrap_data->getPointer(1, d),
                    rho_extrap_data->getPointer(2, d)
#endif
                );
            }

            Pointer<FaceData<NDIM, double> > q_flux_data = patch->getPatchData(d_q_flux_idx);
            const IntVector<NDIM>& q_flux_data_gcw = q_flux_data->getGhostCellWidth();
            Pointer<FaceData<NDIM, double> > rho_flux_data = patch->getPatchData(d_rho_flux_idx);
            const IntVector<NDIM>& rho_flux_data_gcw = rho_flux_data->getGhostCellWidth();
#if !defined(NDEBUG)
            TBOX_ASSERT(rho_flux_data_gcw.min() == rho_flux_data_gcw.max());
#endif
            for (unsigned int d = 0; d < d_Q_data_depth; ++d)
            {
                static const double dt = 1.0;

                ADVECT_FLUX_FC(dt,
#if (NDIM == 2)
                               patch_lower(0),
                               patch_upper(0),
                               patch_lower(1),
                               patch_upper(1),
                               u_ADV_data_gcw(0),
                               u_ADV_data_gcw(1),
                               rho_extrap_data_gcw(0),
                               rho_extrap_data_gcw(1),
                               rho_flux_data_gcw(0),
                               rho_flux_data_gcw(1),
                               u_ADV_data->getPointer(0),
                               u_ADV_data->getPointer(1),
                               rho_extrap_data->getPointer(0, d),
                               rho_extrap_data->getPointer(1, d),
                               rho_flux_data->getPointer(0, d),
                               rho_flux_data->getPointer(1, d)
#endif
#if (NDIM == 3)
                                   patch_lower(0),
                               patch_upper(0),
                               patch_lower(1),
                               patch_upper(1),
                               patch_lower(2),
                               patch_upper(2),
                               u_ADV_data_gcw(0),
                               u_ADV_data_gcw(1),
                               u_ADV_data_gcw(2),
                               rho_extrap_data_gcw(0),
                               rho_extrap_data_gcw(1),
                               rho_extrap_data_gcw(2),
                               rho_flux_data_gcw(0),
                               rho_flux_data_gcw(1),
                               rho_flux_data_gcw(2),
                               u_ADV_data->getPointer(0),
                               u_ADV_data->getPointer(1),
                               u_ADV_data->getPointer(2),
                               rho_extrap_data->getPointer(0, d),
                               rho_extrap_data->getPointer(1, d),
                               rho_extrap_data->getPointer(2, d),
                               rho_flux_data->getPointer(0, d),
                               rho_flux_data->getPointer(1, d),
                               rho_flux_data->getPointer(2, d)
#endif
                );

                ADVECT_FLUX_FC(dt,
#if (NDIM == 2)
                               patch_lower(0),
                               patch_upper(0),
                               patch_lower(1),
                               patch_upper(1),
                               rho_flux_data_gcw(0),
                               rho_flux_data_gcw(1),
                               q_extrap_data_gcw(0),
                               q_extrap_data_gcw(1),
                               q_flux_data_gcw(0),
                               q_flux_data_gcw(1),
                               rho_flux_data->getPointer(0, d),
                               rho_flux_data->getPointer(1, d),
                               q_extrap_data->getPointer(0, d),
                               q_extrap_data->getPointer(1, d),
                               q_flux_data->getPointer(0, d),
                               q_flux_data->getPointer(1, d)
#endif
#if (NDIM == 3)
                                   patch_lower(0),
                               patch_upper(0),
                               patch_lower(1),
                               patch_upper(1),
                               patch_lower(2),
                               patch_upper(2),
                               rho_flux_data_gcw(0),
                               rho_flux_data_gcw(1),
                               rho_flux_data_gcw(2),
                               q_extrap_data_gcw(0),
                               q_extrap_data_gcw(1),
                               q_extrap_data_gcw(2),
                               q_flux_data_gcw(0),
                               q_flux_data_gcw(1),
                               q_flux_data_gcw(2),
                               rho_flux_data->getPointer(0),
                               rho_flux_data->getPointer(1),
                               rho_flux_data->getPointer(2),
                               q_extrap_data->getPointer(0, d),
                               q_extrap_data->getPointer(1, d),
                               q_extrap_data->getPointer(2, d),
                               q_flux_data->getPointer(0, d),
                               q_flux_data->getPointer(1, d),
                               q_flux_data->getPointer(2, d)
#endif
                );
            }
        }
    }

    // Synchronize data on the patch hierarchy.
    for (int ln = d_finest_ln; ln > d_coarsest_ln; --ln)
    {
        d_coarsen_scheds[ln]->coarsenData();
    }
    // Difference values on the patches.
    for (int ln = d_coarsest_ln; ln <= d_finest_ln; ++ln)
    {
        Pointer<PatchLevel<NDIM> > level = d_hierarchy->getPatchLevel(ln);
        for (PatchLevel<NDIM>::Iterator p(level); p; p++)
        {
            Pointer<Patch<NDIM> > patch = level->getPatch(p());

            const Box<NDIM>& patch_box = patch->getBox();
            const IntVector<NDIM>& patch_lower = patch_box.lower();
            const IntVector<NDIM>& patch_upper = patch_box.upper();

            const Pointer<CartesianPatchGeometry<NDIM> > patch_geom = patch->getPatchGeometry();
            const double* const dx = patch_geom->getDx();

            Pointer<CellData<NDIM, double> > N_data = patch->getPatchData(N_idx);
            const IntVector<NDIM>& N_data_gcw = N_data->getGhostCellWidth();

            Pointer<FaceData<NDIM, double> > q_flux_data = patch->getPatchData(d_q_flux_idx);
            const IntVector<NDIM>& q_flux_data_gcw = q_flux_data->getGhostCellWidth();
            for (unsigned int d = 0; d < d_Q_data_depth; ++d)
            {
                static const double alpha = 1.0;

                F_TO_C_DIV_FC(N_data->getPointer(d),
                              N_data_gcw.min(),
                              alpha,
#if (NDIM == 2)
                              q_flux_data->getPointer(0, d),
                              q_flux_data->getPointer(1, d),
                              q_flux_data_gcw.min(),
                              patch_lower(0),
                              patch_upper(0),
                              patch_lower(1),
                              patch_upper(1),
#endif
#if (NDIM == 3)
                              q_flux_data->getPointer(0, d),
                              q_flux_data->getPointer(1, d),
                              q_flux_data->getPointer(2, d),
                              q_flux_data_gcw.min(),
                              patch_lower(0),
                              patch_upper(0),
                              patch_lower(1),
                              patch_upper(1),
                              patch_lower(2),
                              patch_upper(2),
#endif
                              dx);
            }
        }
    }

    // Deallocate scratch data.
    for (int ln = d_coarsest_ln; ln <= d_finest_ln; ++ln)
    {
        Pointer<PatchLevel<NDIM> > level = d_hierarchy->getPatchLevel(ln);
        level->deallocatePatchData(d_Q_scratch_idx);
        level->deallocatePatchData(d_q_extrap_idx);
        level->deallocatePatchData(d_q_flux_idx);

        // for density
        level->deallocatePatchData(d_rho_scratch_idx);
        level->deallocatePatchData(d_rho_extrap_idx);
        level->deallocatePatchData(d_rho_flux_idx);
    }

    IBAMR_TIMER_STOP(t_apply_convective_operator);
    return;
} // applyConvectiveOperator

void
AdvDiffCUIConservativeConvectiveOperator::initializeOperatorState(const SAMRAIVectorReal<NDIM, double>& in,
                                                                  const SAMRAIVectorReal<NDIM, double>& out)
{
    IBAMR_TIMER_START(t_initialize_operator_state);

    if (d_is_initialized) deallocateOperatorState();

    // Get the hierarchy configuration.
    d_hierarchy = in.getPatchHierarchy();
    d_coarsest_ln = in.getCoarsestLevelNumber();
    d_finest_ln = in.getFinestLevelNumber();
#if !defined(NDEBUG)
    TBOX_ASSERT(d_hierarchy == out.getPatchHierarchy());
    TBOX_ASSERT(d_coarsest_ln == out.getCoarsestLevelNumber());
    TBOX_ASSERT(d_finest_ln == out.getFinestLevelNumber());
#else
    NULL_USE(out);
#endif
    Pointer<CartesianGridGeometry<NDIM> > grid_geom = d_hierarchy->getGridGeometry();

    // Setup the coarsen algorithm, operator, and schedules.
    Pointer<CoarsenOperator<NDIM> > coarsen_op = grid_geom->lookupCoarsenOperator(d_q_flux_var, "CONSERVATIVE_COARSEN");
    d_coarsen_alg = new CoarsenAlgorithm<NDIM>();
    // for density
    d_coarsen_alg->registerCoarsen(d_rho_flux_idx, d_rho_flux_idx, coarsen_op);
    d_coarsen_alg->registerCoarsen(d_q_flux_idx, d_q_flux_idx, coarsen_op);
    d_coarsen_scheds.resize(d_finest_ln + 1);
    for (int ln = d_coarsest_ln + 1; ln <= d_finest_ln; ++ln)
    {
        Pointer<PatchLevel<NDIM> > level = d_hierarchy->getPatchLevel(ln);
        Pointer<PatchLevel<NDIM> > coarser_level = d_hierarchy->getPatchLevel(ln - 1);
        d_coarsen_scheds[ln] = d_coarsen_alg->createSchedule(coarser_level, level);
    }

    // Setup the refine algorithm, operator, patch strategy, and schedules.
    Pointer<RefineOperator<NDIM> > refine_op = grid_geom->lookupRefineOperator(d_Q_var, "CONSERVATIVE_LINEAR_REFINE");
    d_ghostfill_alg = new RefineAlgorithm<NDIM>();
    // for density
    d_ghostfill_alg->registerRefine(d_rho_scratch_idx, in.getComponentDescriptorIndex(0), d_rho_scratch_idx, refine_op);
    if (d_outflow_bdry_extrap_type != "NONE")
        d_ghostfill_strategy = new CartExtrapPhysBdryOp(d_rho_scratch_idx, d_outflow_bdry_extrap_type);

    d_ghostfill_alg->registerRefine(d_Q_scratch_idx, in.getComponentDescriptorIndex(0), d_Q_scratch_idx, refine_op);
    if (d_outflow_bdry_extrap_type != "NONE")
        d_ghostfill_strategy = new CartExtrapPhysBdryOp(d_Q_scratch_idx, d_outflow_bdry_extrap_type);

    d_ghostfill_scheds.resize(d_finest_ln + 1);
    for (int ln = d_coarsest_ln; ln <= d_finest_ln; ++ln)
    {
        Pointer<PatchLevel<NDIM> > level = d_hierarchy->getPatchLevel(ln);
        d_ghostfill_scheds[ln] = d_ghostfill_alg->createSchedule(level, ln - 1, d_hierarchy, d_ghostfill_strategy);
    }

    d_is_initialized = true;

    IBAMR_TIMER_STOP(t_initialize_operator_state);
    return;
} // initializeOperatorState

void
AdvDiffCUIConservativeConvectiveOperator::deallocateOperatorState()
{
    if (!d_is_initialized) return;

    IBAMR_TIMER_START(t_deallocate_operator_state);

    // Deallocate the refine algorithm, operator, patch strategy, and schedules.
    d_ghostfill_alg.setNull();
    d_ghostfill_strategy.setNull();
    for (int ln = d_coarsest_ln; ln <= d_finest_ln; ++ln)
    {
        d_ghostfill_scheds[ln].setNull();
    }
    d_ghostfill_scheds.clear();

    d_is_initialized = false;

    IBAMR_TIMER_STOP(t_deallocate_operator_state);
    return;
} // deallocateOperatorState

void
AdvDiffCUIConservativeConvectiveOperator::setMassDensity(const int rho_idx)
{
    d_rho_idx = rho_idx;
    return;
} // setMassDensity

void
AdvDiffCUIConservativeConvectiveOperator::setMassDensityBoundaryConditions(RobinBcCoefStrategy<NDIM>* rho_bc_coef)
{
    for (unsigned int d = 0; d < NDIM; ++d) d_rho_bc_coefs[d] = rho_bc_coef;
    return;
} // setMassDensityBoundaryConditions
void
AdvDiffCUIConservativeConvectiveOperator::setMassDensityVariable(Pointer<Variable<NDIM> > rho_var)
{
    d_rho_var = rho_var;
    return;
} // setMassDensityVariable
/////////////////////////////// PUBLIC ///////////////////////////////////////

/////////////////////////////// PROTECTED ////////////////////////////////////

/////////////////////////////// PRIVATE //////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////

} // namespace IBAMR

//////////////////////////////////////////////////////////////////////////////
