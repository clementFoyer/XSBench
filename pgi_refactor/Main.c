#include "XSbench_header.h"

#ifdef MPI
#include<mpi.h>
#endif

int main( int argc, char* argv[] )
{
  // =====================================================================
  // Initialization & Command Line Read-In
  // =====================================================================
  int version = 13;
  int mype = 0;
#ifndef ACC
  int max_procs = omp_get_num_procs();
#endif
  int i, thread, mat;
  unsigned long seed;
  double tick, tock, p_energy;
  unsigned long long vhash = 0;
  int nprocs;
  double roll;

  //Inputs
  int nthreads;
  long n_isotopes;
  long n_gridpoints;
  int lookups;
  char HM[6];

#ifdef MPI
  MPI_Status stat;
  MPI_Init(&argc, &argv);
  MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
  MPI_Comm_rank(MPI_COMM_WORLD, &mype);
#endif

  // rand() is only used in the serial initialization stages.
  // A custom RNG is used in parallel portions.
#ifdef VERIFICATION
  srand(26);
#else
  srand(time(NULL));
#endif

  // Process CLI Fields
  read_CLI(argc, argv, &nthreads, &n_isotopes, &n_gridpoints, &lookups, HM);

#ifndef ACC
  // Set number of OpenMP Threads
  omp_set_num_threads(nthreads); 
#endif

  // Print-out of Input Summary
  if( mype == 0 )
    print_inputs(nthreads, n_isotopes, n_gridpoints, lookups, HM, nprocs, version);

  // =====================================================================
  // Prepare Nuclide Energy Grids, Unionized Energy Grid, & Material Data
  // =====================================================================

  // Allocate & fill energy grids
#ifndef BINARY_READ
  if( mype == 0) printf("Generating Nuclide Energy Grids...\n");
#endif

  // allocate nuclide_grids[0:n_isotopes][0:n_gridpoints]
  NuclideGridPoint (* restrict nuclide_grids)[n_gridpoints] = 
    (NuclideGridPoint (*)[n_gridpoints]) 
    malloc(n_isotopes * n_gridpoints * sizeof(NuclideGridPoint));

#ifdef VERIFICATION
  generate_grids_v( n_isotopes, n_gridpoints, nuclide_grids );	
#else
  generate_grids( n_isotopes, n_gridpoints, nuclide_grids );	
#endif

  // Sort grids by energy
#ifndef BINARY_READ
  if( mype == 0) printf("Sorting Nuclide Energy Grids...\n");
  sort_nuclide_grids( n_isotopes, n_gridpoints, nuclide_grids );
#endif

  // Prepare Unionized Energy Grid Framework
  int * restrict grid_ptrs = generate_ptr_grid(n_isotopes, n_gridpoints);
#ifndef BINARY_READ
  GridPoint * restrict energy_grid = generate_energy_grid( n_isotopes,
      n_gridpoints, nuclide_grids, grid_ptrs ); 	
#else
  GridPoint * restrict energy_grid = (GridPoint *)malloc( n_isotopes *
      n_gridpoints * sizeof( GridPoint ) );
  for( i = 0; i < n_isotopes*n_gridpoints; i++ )
    energy_grid[i].xs_ptrs = i*n_isotopes;
#endif

  // Double Indexing. Filling in energy_grid with pointers to the
  // nuclide_energy_grids.
#ifndef BINARY_READ
  set_grid_ptrs( energy_grid, grid_ptrs, n_isotopes, n_gridpoints, nuclide_grids);
#endif

#ifdef BINARY_READ
  if( mype == 0 ) printf("Reading data from \"XS_data.dat\" file...\n");
  binary_read(n_isotopes, n_gridpoints, nuclide_grids, energy_grid, grid_ptrs);
#endif

  // Get material data
  if( mype == 0 )
    printf("Loading Mats...\n");

  int size_mats;
  if (n_isotopes == 68) 
    size_mats = 197;
  else
    size_mats = 484;

  int * restrict num_nucs  = load_num_nucs(n_isotopes);
  int * restrict mats_idx  = load_mats_idx(num_nucs);
  int * restrict mats      = load_mats( num_nucs, mats_idx, size_mats, n_isotopes );

#ifdef VERIFICATION
  double * restrict concs = load_concs_v(size_mats);
#else
  double * restrict concs = load_concs(size_mats);
#endif

#ifdef BINARY_DUMP
  if( mype == 0 ) printf("Dumping data to binary file...\n");
  binary_dump(n_isotopes, n_gridpoints, nuclide_grids, energy_grid, grid_ptrs);
  if( mype == 0 ) printf("Binary file \"XS_data.dat\" written! Exiting...\n");
  return 0;
#endif

  // =====================================================================
  // Cross Section (XS) Parallel Lookup Simulation Begins
  // =====================================================================


  if( mype == 0 )
  {
    printf("\n");
    border_print();
    center_print("SIMULATION", 79);
    border_print();
  }

#ifdef ACC
  tick = timer();
#else
  tick = omp_get_wtime();
#endif


  // OpenMP compiler directives - declaring variables as shared or private
#ifdef ACC
#pragma acc data \
  copy(vhash) \
  copyin( \
      n_isotopes, \
      n_gridpoints, \
      lookups, \
      energy_grid[0:n_isotopes*n_gridpoints], \
      nuclide_grids[0:n_isotopes][0:n_gridpoints], \
      grid_ptrs[0:n_isotopes*n_isotopes*n_gridpoints], \
      mats[0:size_mats], \
      mats_idx[0:12], \
      concs[0:size_mats], \
      num_nucs[0:12] )
#else
#pragma omp parallel default(none) \
  private(i, thread, p_energy, mat, seed, roll) \
  shared( \
      max_procs, \
      nthreads, \
      n_isotopes, \
      n_gridpoints, \
      lookups, \
      energy_grid, \
      nuclide_grids, \
      grid_ptrs, \
      mats, \
      mats_idx, \
      concs, \
      num_nucs, \
      mype, \
      vhash) 
#endif
  {	

    double macro_xs_vector[5];

    // Initialize RNG seeds for threads
#ifndef ACC
    thread = omp_get_thread_num();
    seed   = (thread+1)*19+17;
#endif

    // XS Lookup Loop
#ifdef ACC
#pragma acc parallel for private(seed)
#else
#pragma omp for schedule(dynamic)
#endif
    for( i = 0; i < lookups; i++ )
    {
      // Status text
#ifndef ACC
      if( INFO && mype == 0 && thread == 0 && i % 1000 == 0 )
        printf("\rCalculating XS's... (%.0lf%% completed)",
            (i / ( (double)lookups / (double) nthreads ))
            / (double) nthreads * 100.0);
#endif

#ifdef ACC
    seed   = (i+1)*19+17;
#endif

      // Randomly pick an energy and material for the particle
#ifdef VERIFICATION
#ifndef ACC
#pragma omp critical
      {
        p_energy = rn_v();
        roll = rn_v();
      }
#endif
#else
      p_energy = rn(&seed);
      roll = rn(&seed);
#endif
      // INLINE:  pick_mat(mat_roll)
      {
        // I have a nice spreadsheet supporting these numbers. They are
        // the fractions (by volume) of material in the core. Not a 
        // *perfect* approximation of where XS lookups are going to occur,
        // but this will do a good job of biasing the system nonetheless.

        // Also could be argued that doing fractions by weight would be 
        // a better approximation, but volume does a good enough job for now.

        double dist[12];
        dist[0]  = 0.140;	// fuel
        dist[1]  = 0.052;	// cladding
        dist[2]  = 0.275;	// cold, borated water
        dist[3]  = 0.134;	// hot, borated water
        dist[4]  = 0.154;	// RPV
        dist[5]  = 0.064;	// Lower, radial reflector
        dist[6]  = 0.066;	// Upper reflector / top plate
        dist[7]  = 0.055;	// bottom plate
        dist[8]  = 0.008;	// bottom nozzle
        dist[9]  = 0.015;	// top nozzle
        dist[10] = 0.025;	// top of fuel assemblies
        dist[11] = 0.013;	// bottom of fuel assemblies

        // makes a pick based on the distro
        // for( int i = 0; i < 12; i++ )
        // {
        // 	double running = 0;
        // 	for( int j = i; j > 0; j-- )
        // 		running += dist[j];
        // 	if( roll < running )
        // 		return i;
        // }

        // return 0;

        for( mat = 0; mat < 12; mat++ )
        {
          double running = 0;
          for(int j = mat; j > 0; j-- )
            running += dist[j];
          if( roll < running )
            break;
        }
        mat = mat % 12;
      }

      assert(mat < 12);
      assert(mat >= 0);

      // debugging
      //printf("E = %lf mat = %d\n", p_energy, mat);

      // This returns the macro_xs_vector, but we're not going
      // to do anything with it in this program, so return value
      // is written over.
      // INLINE: calculate_macro_xs( p_energy, mat, n_isotopes,
      //     n_gridpoints, num_nucs, concs,
      //     energy_grid, grid_ptrs, nuclide_grids, mats, mats_idx,
      //     macro_xs_vector );
      {
        double xs_vector[5];
        int p_nuc; // the nuclide we are looking up
        long idx = 0;	
        double conc; // the concentration of the nuclide in the material

        // cleans out macro_xs_vector
        for( int k = 0; k < 5; k++ )
          macro_xs_vector[k] = 0;

        // binary search for energy on unionized energy grid (UEG)
        idx = grid_search( n_isotopes * n_gridpoints, p_energy,
            energy_grid);	

        // Once we find the pointer array on the UEG, we can pull the data
        // from the respective nuclide grids, as well as the nuclide
        // concentration data for the material
        // Each nuclide from the material needs to have its micro-XS array
        // looked up & interpolatied (via calculate_micro_xs). Then, the
        // micro XS is multiplied by the concentration of that nuclide
        // in the material, and added to the total macro XS array.
        for( int j = 0; j < num_nucs[mat]; j++ ) {
          p_nuc = mats[mats_idx[mat] + j];
          conc = concs[mats_idx[mat] + j];

          // INLINE: calculate_micro_xs( p_energy, p_nuc, n_isotopes,
          //     n_gridpoints, energy_grid, grid_ptrs,
          //     nuclide_grids, idx, xs_vector );
          {

            // Variables
            double f;
            NuclideGridPoint * low, * high;

            // pull ptr from energy grid and check to ensure that
            // we're not reading off the end of the nuclide's grid
            if( grid_ptrs[energy_grid[idx].xs_ptrs + p_nuc] == n_gridpoints - 1 )
              low = &nuclide_grids[p_nuc][grid_ptrs[energy_grid[idx].xs_ptrs + p_nuc] - 1];
            else
              low = &nuclide_grids[p_nuc][grid_ptrs[energy_grid[idx].xs_ptrs + p_nuc]];

            high = low + 1;

            // calculate the re-useable interpolation factor
            f = (high->energy - p_energy) / (high->energy - low->energy);

            // Total XS
            xs_vector[0] = high->total_xs - f * (high->total_xs - low->total_xs);

            // Elastic XS
            xs_vector[1] = high->elastic_xs - f * (high->elastic_xs - low->elastic_xs);

            // Absorbtion XS
            xs_vector[2] = high->absorbtion_xs - f * (high->absorbtion_xs - low->absorbtion_xs);

            // Fission XS
            xs_vector[3] = high->fission_xs - f * (high->fission_xs - low->fission_xs);

            // Nu Fission XS
            xs_vector[4] = high->nu_fission_xs - f * (high->nu_fission_xs - low->nu_fission_xs);

            //test
            /*	
                if( omp_get_thread_num() == 0 )
                {
                printf("Lookup: Energy = %lf, nuc = %d\n", p_energy, nuc);
                printf("e_h = %lf e_l = %lf\n", high->energy , low->energy);
                printf("xs_h = %lf xs_l = %lf\n", high->elastic_xs, low->elastic_xs);
                printf("total_xs = %lf\n\n", xs_vector[1]);
                }
                */

          }
          for( int k = 0; k < 5; k++ )
            macro_xs_vector[k] += xs_vector[k] * conc;
        }

        //test
        /*
           for( int k = 0; k < 5; k++ )
           printf("Energy: %lf, Material: %d, XSVector[%d]: %lf\n",
           p_energy, mat, k, macro_xs_vector[k]);
           */
      }

      // Verification hash calculation
      // This method provides a consistent hash accross
      // architectures and compilers.
#ifdef VERIFICATION
      char line[256];
      sprintf(line, "%.5lf %d %.5lf %.5lf %.5lf %.5lf %.5lf",
          p_energy, mat,
          macro_xs_vector[0],
          macro_xs_vector[1],
          macro_xs_vector[2],
          macro_xs_vector[3],
          macro_xs_vector[4]);
      unsigned long long vhash_local = hash(line, 10000);
#ifndef ACC
#pragma omp atomic
#endif
      vhash += vhash_local;
#endif
    }


  }

#ifdef ACC
  tock = timer();
#else
  tock = omp_get_wtime();
#endif

  // Print / Save Results and Exit
  print_results(nthreads, n_isotopes, n_gridpoints, lookups, HM, mype, tock-tick, nprocs, vhash);


#ifdef MPI
  MPI_Finalize();
#endif

  return 0;
}