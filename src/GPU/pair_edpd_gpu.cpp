/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   https://www.lammps.org/, Sandia National Laboratories
   LAMMPS development team: developers@lammps.org

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   Contributing author: Trung Dac Nguyen (U Chicago)
------------------------------------------------------------------------- */

#include "pair_edpd_gpu.h"

#include "atom.h"
#include "domain.h"
#include "error.h"
#include "force.h"
#include "gpu_extra.h"
#include "info.h"
#include "neigh_list.h"
#include "neighbor.h"
#include "suffix.h"
#include "update.h"

#include <cmath>

using namespace LAMMPS_NS;

// External functions from cuda library for atom decomposition

int edpd_gpu_init(const int ntypes, double **cutsq, double **host_a0, double **host_gamma,
                  double **host_cut, double **host_power, double **host_kappa,
                  double **host_powerT, double** host_cutT, double*** host_sc, double ***host_kc,
                  double *host_mass, double *special_lj, const int power_flag, const int kappa_flag,
                  const int inum, const int nall, const int max_nbors,
                  const int maxspecial, const double cell_size, int &gpu_mode, FILE *screen);
void edpd_gpu_clear();
int **edpd_gpu_compute_n(const int ago, const int inum_full, const int nall, double **host_x,
                        int *host_type, double *sublo, double *subhi, tagint *tag, int **nspecial,
                        tagint **special, const bool eflag, const bool vflag, const bool eatom,
                        const bool vatom, int &host_start, int **ilist, int **jnum,
                        const double cpu_time, bool &success, double **host_v,
                        const double dtinvsqrt, const int seed, const int timestep, double *boxlo,
                        double *prd);
void edpd_gpu_compute(const int ago, const int inum_full, const int nall, double **host_x,
                     int *host_type, int *ilist, int *numj, int **firstneigh, const bool eflag,
                     const bool vflag, const bool eatom, const bool vatom, int &host_start,
                     const double cpu_time, bool &success, tagint *tag, double **host_v,
                     const double dtinvsqrt, const int seed, const int timestep, const int nlocal,
                     double *boxlo, double *prd);
void edpd_gpu_get_extra_data(double *host_T, double *host_cv);
void edpd_gpu_update_flux(void **flux_ptr);
double edpd_gpu_bytes();

#define EPSILON 1.0e-10

/* ---------------------------------------------------------------------- */

PairEDPDGPU::PairEDPDGPU(LAMMPS *lmp) : PairEDPD(lmp), gpu_mode(GPU_FORCE)
{
  flux_pinned = nullptr;
  respa_enable = 0;
  reinitflag = 0;
  cpu_time = 0.0;
  suffix_flag |= Suffix::GPU;
  GPU_EXTRA::gpu_ready(lmp->modify, lmp->error);
}

/* ----------------------------------------------------------------------
   free all arrays
------------------------------------------------------------------------- */

PairEDPDGPU::~PairEDPDGPU()
{
  edpd_gpu_clear();
}

/* ---------------------------------------------------------------------- */

void PairEDPDGPU::compute(int eflag, int vflag)
{
  ev_init(eflag, vflag);

  int nall = atom->nlocal + atom->nghost;
  int inum, host_start;

  double dtinvsqrt = 1.0 / sqrt(update->dt);

  bool success = true;
  int *ilist, *numneigh, **firstneigh;

  double *T = atom->edpd_temp;
  double *cv = atom->edpd_cv;
  edpd_gpu_get_extra_data(T, cv);

  if (gpu_mode != GPU_FORCE) {
    double sublo[3], subhi[3];
    if (domain->triclinic == 0) {
      sublo[0] = domain->sublo[0];
      sublo[1] = domain->sublo[1];
      sublo[2] = domain->sublo[2];
      subhi[0] = domain->subhi[0];
      subhi[1] = domain->subhi[1];
      subhi[2] = domain->subhi[2];
    } else {
      domain->bbox(domain->sublo_lamda, domain->subhi_lamda, sublo, subhi);
    }
    inum = atom->nlocal;
    firstneigh = edpd_gpu_compute_n(
        neighbor->ago, inum, nall, atom->x, atom->type, sublo, subhi, atom->tag, atom->nspecial,
        atom->special, eflag, vflag, eflag_atom, vflag_atom, host_start, &ilist, &numneigh,
        cpu_time, success, atom->v, dtinvsqrt, seed, update->ntimestep, domain->boxlo, domain->prd);
  } else {
    inum = list->inum;
    ilist = list->ilist;
    numneigh = list->numneigh;
    firstneigh = list->firstneigh;
    edpd_gpu_compute(neighbor->ago, inum, nall, atom->x, atom->type, ilist, numneigh, firstneigh,
                    eflag, vflag, eflag_atom, vflag_atom, host_start, cpu_time, success, atom->tag,
                    atom->v, dtinvsqrt, seed, update->ntimestep, atom->nlocal, domain->boxlo, domain->prd);
  }
  if (!success) error->one(FLERR, "Insufficient memory on accelerator");

  // get the heat flux from device

  double *Q = atom->edpd_flux;
  edpd_gpu_update_flux(&flux_pinned);

  int nlocal = atom->nlocal;
  if (acc_float) {
    auto flux_ptr = (float *)flux_pinned;
    for (int i = 0; i < nlocal; i++)
      Q[i] = flux_ptr[i];

  } else {
    auto flux_ptr = (double *)flux_pinned;
    for (int i = 0; i < nlocal; i++)
      Q[i] = flux_ptr[i];
  }

  if (atom->molecular != Atom::ATOMIC && neighbor->ago == 0)
    neighbor->build_topology();
}

/* ----------------------------------------------------------------------
   init specific to this pair style
------------------------------------------------------------------------- */

void PairEDPDGPU::init_style()
{

  // Repeat cutsq calculation because done after call to init_style
  double maxcut = -1.0;
  double mcut;
  for (int i = 1; i <= atom->ntypes; i++) {
    for (int j = i; j <= atom->ntypes; j++) {
      if (setflag[i][j] != 0 || (setflag[i][i] != 0 && setflag[j][j] != 0)) {
        mcut = init_one(i, j);
        mcut *= mcut;
        if (mcut > maxcut) maxcut = mcut;
        cutsq[i][j] = cutsq[j][i] = mcut;
      } else
        cutsq[i][j] = cutsq[j][i] = 0.0;
    }
  }
  double cell_size = sqrt(maxcut) + neighbor->skin;

  int maxspecial = 0;
  if (atom->molecular != Atom::ATOMIC) maxspecial = atom->maxspecial;
  int mnf = 5e-2 * neighbor->oneatom;
  int success =
      edpd_gpu_init(atom->ntypes + 1, cutsq, a0, gamma, cut, power, kappa,
                    powerT, cutT, sc, kc, atom->mass, force->special_lj,
                    power_flag, kappa_flag, atom->nlocal, atom->nlocal + atom->nghost,
                    mnf, maxspecial, cell_size, gpu_mode, screen);
  GPU_EXTRA::check_flag(success, error, world);

  acc_float = Info::has_accelerator_feature("GPU", "precision", "single");

  if (gpu_mode == GPU_FORCE) neighbor->add_request(this, NeighConst::REQ_FULL);
}

/* ---------------------------------------------------------------------- */

double PairEDPDGPU::memory_usage()
{
  double bytes = Pair::memory_usage();
  return bytes + edpd_gpu_bytes();
}
