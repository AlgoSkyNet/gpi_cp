/*
Copyright (c) Fraunhofer ITWM 

This file is part of gpi_cp.

gpi_cp is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License
version 3 as published by the Free Software Foundation.

gpi_cp is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with gpi_cp. If not, see <http://www.gnu.org/licenses/>.
*/


#include <GASPI.h>
#include <gpi_cp.h>

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

//#define DEBUG 1
#define WITH_CHECKPOINT 1
#ifdef WITH_CHECKPOINT
#define SPARE_RANKS 1
#else
#define SPARE_RANKS 0
#endif


#define ASSERT(ec) success_or_exit (__FILE__, __LINE__, ec);

void success_or_exit ( const char* file, const int line, const int ec)
{
  if (ec != GASPI_SUCCESS)
    {
      gaspi_printf ("Assertion failed in %s[%i]:%d\n", file, line, ec);
      
      exit (EXIT_FAILURE);
    }
}


/* example stencil:

   let sh be the stencil_height, then the new state at position (x,y)
   is computed as
   (\sum_{x-sh <= x' <= x+sh}_{y-sh <= y' <= y+sh} state (x',y')) mod aprime
*/

#define INDEX(x, y) ((x) + size_global_x * (y))

typedef unsigned int element_type;

static gaspi_size_t
  stencil_height()
{
  return 2;
}

static element_type
  prime()
{
  return 100003;
}

static void
  step ( element_type *from, element_type *to
       , gaspi_size_t size_global_x
       , gaspi_size_t begin_local_y, gaspi_size_t end_local_y
       )
{
  for (gaspi_size_t y = begin_local_y; y < end_local_y; ++y)
  {
    for (gaspi_size_t x = 0; x < size_global_x; ++x)
    {
      element_type sum = 0;

      for ( gaspi_size_t y_from = y - stencil_height()
          ; y_from <= y + stencil_height()
          ; ++y_from
          )
      {
        for ( gaspi_size_t x_shift = 0
            ; x_shift <= 2 * stencil_height()
            ; ++x_shift
            )
        {
          gaspi_size_t const x_from =
            (x + size_global_x + x_shift - stencil_height()) % size_global_x;

          sum += from[INDEX (x_from, y_from)];
        }
      }

      to[INDEX (x, y)] = sum % prime();
    }
  }
}

#define STEP(begin_local_y, end_local_y)        \
  step ( buffer[from], buffer[to]               \
       , size_global_x                          \
       , begin_local_y, end_local_y             \
       )


static void
  initialize ( element_type* data
             , gaspi_size_t size_global_x
             , gaspi_size_t begin_global_y, gaspi_size_t end_global_y
             )
{
  for (gaspi_size_t y = begin_global_y; y < end_global_y; ++y)
  {
    for (gaspi_size_t x = 0; x < size_global_x; ++x)
    {
      data[INDEX (x, y - begin_global_y)] = rand() % prime();
    }
  }
}

#define ERROR(message)                          \
  do                                            \
  {                                             \
    gaspi_printf ( "Error[%s:%i]: %s\n"         \
                 , __FILE__, __LINE__, message  \
                 );                             \
                                                \
    exit (EXIT_FAILURE);                        \
  } while (0)


#define SUCCESS_OR_DIE(f, args...)		\
  do						\
  {						\
    gaspi_return_t const r = f (args);	\
						\
    if (r != GASPI_SUCCESS)			\
    {						\
      ERROR (gaspi_error_str (r));		\
    }						\
  } while (0)


/* wait for a number of entries to be available in the communication queue

   - if a queue has not enough space, the next one is taken
   - in the new queue a wait is done to ensure it is empty
   - implements the "wait as late as possible" approach
*/
static void
  wait_for_queue_entries ( gaspi_queue_id_t* queue
                         , gaspi_number_t wanted_entries
                         )
{
  gaspi_number_t queue_size_max;
  gaspi_number_t queue_size;
  gaspi_number_t queue_num;

  SUCCESS_OR_DIE (gaspi_queue_size_max, &queue_size_max);
  SUCCESS_OR_DIE (gaspi_queue_size, *queue, &queue_size);
  SUCCESS_OR_DIE (gaspi_queue_num, &queue_num);

  if (! (queue_size + wanted_entries <= queue_size_max))
  {
    *queue = (*queue + 1) % queue_num;

    SUCCESS_OR_DIE (gaspi_wait, *queue, GASPI_BLOCK);
  }
}

double
euclidean_norm( gaspi_group_t group, element_type *buf, int vec_length )
{
  double m_sum = 0.0, t_sum = 0.0;
  for (int i = 0; i < vec_length; ++i )
    {
      m_sum  += buf[i]^2;
    }

  /* gaspi_printf("My norm %.2f %d\n", m_sum, vec_length); */
  
  SUCCESS_OR_DIE(gaspi_allreduce
		 , &m_sum
		 , &t_sum
		 , 1
		 , GASPI_OP_SUM
		 , GASPI_TYPE_DOUBLE
		 , group
		 , GASPI_BLOCK);

  return sqrt(t_sum);
}

/* when distribute m element on p slots, this function can be used to
   determine the ordinal of the first element on slot n, e.g. slot n
   contains the range [begin (m, p, n), begin (m, p, n + 1))
*/
static gaspi_size_t
  begin (gaspi_size_t m, gaspi_size_t p, gaspi_size_t n)
{
  return (n * m + p - 1) / p;
}

/* Create a group with total_nprocs - number_of_reserve_procs */
gaspi_group_t
cp_group_create(int number_of_reserve_processes)
{
  gaspi_group_t group_active;
 
  gaspi_number_t gsize, gsize_active; 
  gaspi_rank_t *group_ranks;
  gaspi_rank_t rank;
  ASSERT (gaspi_proc_rank(&rank));

  ASSERT(gaspi_group_size(GASPI_GROUP_ALL, &gsize));

  group_ranks = malloc(gsize * sizeof(gaspi_rank_t));
  ASSERT(gaspi_group_ranks(GASPI_GROUP_ALL, group_ranks));

#ifdef DEBUG
    gaspi_printf("debug: number of ranks: %i, number of reserve processes: %i \n",
		 gsize, number_of_reserve_processes);
#endif

  group_active = 0;
  if(rank < gsize - number_of_reserve_processes)
    {
      ASSERT(gaspi_group_create(&group_active));

      int i;
      for (i = 0; i < (int) (gsize - number_of_reserve_processes); i++) 
	ASSERT(gaspi_group_add(group_active, group_ranks[i]));

      ASSERT(gaspi_group_size(group_active, &gsize_active));

#ifdef DEBUG
      gaspi_printf("debug: group_active has %i out of %i processes attached\n",
		   gsize_active, gsize);
#endif
      if(rank < gsize_active)
	ASSERT(gaspi_group_commit(group_active, GASPI_BLOCK));
    }

  free(group_ranks);

  return group_active;
}

gaspi_return_t
g_create_group(gaspi_rank_t nprocs, gaspi_group_t *g, gaspi_rank_t avoid)
{
  gaspi_rank_t myrank, nranks;

  SUCCESS_OR_DIE(gaspi_proc_rank, &myrank);
  SUCCESS_OR_DIE(gaspi_proc_num, &nranks);
  
  SUCCESS_OR_DIE (gaspi_group_create, g);

  gaspi_rank_t added = 0;
  gaspi_rank_t ra = 0;

  while( added < nprocs && ra < nranks)
    {
      if(ra != avoid)
	{
	  SUCCESS_OR_DIE(gaspi_group_add, *g, ra);
	  added++;
	}
      ra++;
    }

  if(added != nprocs)
    return GASPI_ERROR;
 
  SUCCESS_OR_DIE(gaspi_group_commit, *g, GASPI_BLOCK);

  return GASPI_SUCCESS;
}

int
__is_in_group (gaspi_group_t group, gaspi_rank_t rank)
{
  gaspi_number_t size;

  if ( GASPI_SUCCESS != gaspi_group_size (group, &size) )
    return 0;

  gaspi_rank_t *ranks = malloc (size * sizeof (gaspi_rank_t));

  if ( GASPI_SUCCESS != gaspi_group_ranks (group, ranks) )
    return 1;

  gaspi_number_t i;
  for (i = 0; i < size; ++i)
  {
    if (ranks[i] == rank)
      return 1;
  }

  return 0;
}

gaspi_group_t
simulate_fault(gaspi_group_t old_group, int *is_active, gaspi_rank_t culprit)
{
  gaspi_group_t new_group;
  gaspi_rank_t myrank;

  SUCCESS_OR_DIE( gaspi_proc_rank, &myrank);

  /* make them wait, specially the spare nodes that do nothing */
  gaspi_barrier(GASPI_GROUP_ALL, GASPI_BLOCK);
  /* Simulate a fault */
  gaspi_printf("A FAULT (group %u active %d culprit %d)\n",  old_group, *is_active, culprit);

  new_group = 0;
  if(myrank != culprit )
    {
      if( *is_active )
	{
	  gaspi_number_t nranks;
	  SUCCESS_OR_DIE( gaspi_group_size, old_group, &nranks);

	  /* delete old group */
	  SUCCESS_OR_DIE(gaspi_group_delete, old_group);

	  /* Create the new group */
	  SUCCESS_OR_DIE(g_create_group, nranks, &new_group, culprit);
	}
      else
	{
	  gaspi_rank_t nranks;
	  SUCCESS_OR_DIE( gaspi_proc_num, &nranks);

	  if(myrank == nranks - SPARE_RANKS)
	    {
	      /* Create the new group */
	      SUCCESS_OR_DIE(g_create_group, nranks - SPARE_RANKS, &new_group, culprit);
	      *is_active = 1;
	    }
	}
    }
  else
    {
      /* I'm failing, jump out */
      _exit(-1);
    }

  return new_group;
}

int
main(int argc, char *argv[])
{
  double compute_time, total_time;
  struct timeval tcompute_start, tcompute_end;
  struct timeval ttotal_start, ttotal_end;
  int checkpoint_cycle = 20;


  gaspi_size_t const size_global_x = 1913;//997;
  gaspi_size_t const size_global_y = 2017;//1009;

  gettimeofday(&ttotal_start, NULL);

  unsigned const iteration = 49;

  if( argc > 1 )
    checkpoint_cycle = atoi(argv[1]);

  gaspi_printf("Checkpoint interval: %d iterations %d\n", checkpoint_cycle, iteration);
  gettimeofday(&ttotal_start, NULL);

  SUCCESS_OR_DIE (gaspi_proc_init, GASPI_BLOCK);

  gaspi_rank_t iProc;
  gaspi_rank_t nProc;

  SUCCESS_OR_DIE (gaspi_proc_rank, &iProc);
  SUCCESS_OR_DIE (gaspi_proc_num, &nProc);
  gaspi_group_t group_active = 23;

#ifdef WITH_CHECKPOINT
  int rank_is_active = ( iProc < (nProc - SPARE_RANKS));

  gaspi_group_t new_group;
  gpi_cp_description_t checkpoint_description =  GPI_CP_DESCRIPTION_INITIALIZER();
  gaspi_pointer_t checkpoint_seg_ptr;

  if(rank_is_active)
    {
      group_active = cp_group_create(SPARE_RANKS);
    }
  new_group = group_active;
#else
  int rank_is_active = true;
  group_active = GASPI_GROUP_ALL;
#endif

  gaspi_size_t  begin_global_y = begin (size_global_y, nProc - SPARE_RANKS, iProc);
  gaspi_size_t  end_global_y = begin (size_global_y, nProc - SPARE_RANKS, iProc + 1);
  gaspi_size_t  size_local_y = end_global_y - begin_global_y;

  if (size_local_y < 2 * stencil_height())
    {
      ERROR ("local size smaller than stencil height, use less ranks");
    }

  gaspi_segment_id_t segment_id[2];
  gaspi_pointer_t segment_pointer[2] = {0, 0};

  gaspi_segment_id_t* unused_segment_id;
  gaspi_segment_id_t tmp=10;
  unused_segment_id=&tmp;

  for (int i = 0; i < 2; ++i)
    {
      SUCCESS_OR_DIE(gpi_cp_get_unused_segment_id, unused_segment_id);

#ifdef DEBUG
      gaspi_printf("unused_segment_id %i \n", *unused_segment_id );
#endif
      segment_id[i]=*unused_segment_id;

      SUCCESS_OR_DIE
	( gaspi_segment_create
	  , segment_id[i]
	  /* halo area above and below */
	  , size_global_x * (size_local_y + 2 * stencil_height()) * sizeof (element_type)
	  , GASPI_GROUP_ALL
	  , GASPI_BLOCK
	  , GASPI_MEM_INITIALIZED
	  );

      SUCCESS_OR_DIE (gaspi_segment_ptr, segment_id[i], segment_pointer + i);
    }

#ifdef WITH_CHECKPOINT
  SUCCESS_OR_DIE(gpi_cp_get_unused_segment_id, unused_segment_id);

  unsigned long mysize = size_global_x * (size_local_y + 2 * stencil_height()) * sizeof (element_type);
  unsigned long maxsize = 0;

  SUCCESS_OR_DIE (gaspi_allreduce
		  , &mysize
		  , &maxsize
		  , 1
		  , GASPI_OP_MAX
		  , GASPI_TYPE_ULONG
		  , GASPI_GROUP_ALL
		  , GASPI_BLOCK);

  gaspi_printf("SIZES mine %lu max %u\n", mysize, maxsize);
  
  /* All create segment to be checkpointed */
  SUCCESS_OR_DIE (gaspi_segment_create
		  , *unused_segment_id
		  , maxsize
		  , GASPI_GROUP_ALL
		  , GASPI_BLOCK
		  , GASPI_MEM_UNINITIALIZED);

  /* Only the active ranks initialize the checkpointing */
  if(rank_is_active)
  {
      SUCCESS_OR_DIE( gpi_cp_init,
                      *unused_segment_id
                      , (gaspi_offset_t) 0
                      , maxsize
                      , (gaspi_queue_id_t) 4
                      , GPI_CP_POLICY_RING
                      , group_active
                      , checkpoint_description
                      , GASPI_BLOCK );
  }

  gaspi_segment_ptr(*unused_segment_id, &checkpoint_seg_ptr);
#endif

  element_type* buffer[2] = {segment_pointer[0], segment_pointer[1]};

  /* real data start after the halo area */
  element_type* data[2] = { buffer[0] + size_global_x * stencil_height()
			    , buffer[1] + size_global_x * stencil_height()
  };

  //  if(rank_is_active)
    initialize (data[0], size_global_x, begin_global_y, end_global_y);

  gaspi_rank_t iAbove = (iProc - 1 + (nProc - SPARE_RANKS)) % (nProc - SPARE_RANKS);
  gaspi_rank_t iBelow = (iProc + 1 + (nProc - SPARE_RANKS)) % (nProc - SPARE_RANKS);

  /* single sided communication requires to know about the remote situation
     remember that the distribution might be not symmetric
  */
  gaspi_size_t  size_local_y_above =
    begin (size_global_y, (nProc - SPARE_RANKS), iAbove + 1)
    - begin (size_global_y, (nProc - SPARE_RANKS), iAbove);
  
  gaspi_notification_id_t const flag_from_above = 0;
  gaspi_notification_id_t const flag_from_below = 1;

  gaspi_queue_id_t queue = 0;

  SUCCESS_OR_DIE (gaspi_barrier, GASPI_GROUP_ALL, GASPI_BLOCK);

#ifdef DEBUG
#ifdef WITH_CHECKPOINT
  double init_norm1, init_norm2;
  if(rank_is_active)
    {
      init_norm1 = euclidean_norm(group_active, data[0], size_global_x * size_local_y);
      gaspi_printf("init xNorm %.2f\n", init_norm1);
    } 
#endif
#endif
  
  gettimeofday(&tcompute_start, NULL);
  int faulted = 0;
  for (unsigned k = 0; k < iteration; ++k)
    {
      unsigned const from = k % 2;
      unsigned const to = 1 - from;

#ifdef WITH_CHECKPOINT
    if( (k % checkpoint_cycle ) == 0 )
      {
        if(rank_is_active)
        {
  	  /* Commit previously started checkpoint */
          SUCCESS_OR_DIE (gpi_cp_commit, checkpoint_description, GASPI_BLOCK);

          /* Save data to be checkpointed */
          memcpy(checkpoint_seg_ptr, buffer[from], size_global_x * (size_local_y + 2 * stencil_height()) * sizeof (element_type));
          
          /* Start a new checkpoint */
          SUCCESS_OR_DIE ( gpi_cp_start, checkpoint_description, GASPI_BLOCK ) ;
        }
      }
#endif

#ifdef WITH_CHECKPOINT
      /* Fault simulation */
      if( k == 33 && !faulted )
      	{
	  int before_active = rank_is_active;

	  new_group = simulate_fault(group_active, &rank_is_active, nProc - 1 - SPARE_RANKS);

      	  /* Do Restore */
      	  if(rank_is_active)
      	    {
      	      /* gaspi_printf("Start restore %d\n", k); */
      	      SUCCESS_OR_DIE(gpi_cp_restore
      	      		     , *unused_segment_id
      	      		     , 0
      	      		     , maxsize
      	      		     , 4
      	      		     , GPI_CP_POLICY_RING
      	      		     , new_group
      	      		     , checkpoint_description
                             , GASPI_BLOCK
      	      		     );

	      /* in case of asymmetric data and checkpoints we need to
		 set the joiner (before not active) with the same
		 sizes as the culprit of the fault .
		 It's hackish: need to think about this better. */
	      if( !before_active )
	      	{
		  gaspi_rank_t culprit =  nProc - 1 - SPARE_RANKS;
		  begin_global_y = begin (size_global_y, nProc - SPARE_RANKS, culprit);
		  end_global_y = begin (size_global_y, nProc - SPARE_RANKS, culprit + 1);
		  size_local_y = end_global_y - begin_global_y;
		  size_local_y_above = 
		    begin (size_global_y, (nProc - SPARE_RANKS), iAbove + 1)
		    - begin (size_global_y, (nProc - SPARE_RANKS), iAbove);
	      	}

      	      /* copy checkpointed data */
	      memcpy( buffer[0], checkpoint_seg_ptr, size_global_x * (size_local_y + 2 * stencil_height()) * sizeof (element_type));

      	      /* Update neighbourhood: assumes ring topology */
      	      iAbove = (iProc - 1 + nProc ) % (nProc);
      	      while( !__is_in_group (new_group, iAbove) )
      	      	iAbove = (iAbove - 1 + nProc) % nProc ;

      	      iBelow = (iProc + 1 + nProc ) % nProc;
      	      while( !__is_in_group(new_group, iBelow))
      	      	iBelow = (iBelow + 1 + nProc ) % nProc;

      	      group_active = new_group;

#ifdef DEBUG
	      if(rank_is_active)
		{
 		  double norm1;
		  if(iProc == 7)
		    norm1 = euclidean_norm(group_active, data[0], 550944);
		  else
		    norm1 = euclidean_norm(group_active, data[0], size_global_x * size_local_y);
		  if(norm1 != init_norm1)
		    gaspi_printf("checkpoint norm different than init %.2f %.2f\n", 
				 norm1, init_norm1);
		} 
#endif

	      /* update iteration: go back to a safe one */
	      k =  k - (k % checkpoint_cycle) - 1 ;

	      /* For now we allow one fault <=> one spare rank */
	      faulted = 1;

	      continue;
      	    }
      	}
#endif


      if(rank_is_active)
	{
#ifdef WITH_CHECKPOINT
          /* Do checkpoints */
	  if( (k % checkpoint_cycle ) == 0 )
	    {
	      /* Commit previously started checkpoint */
	      SUCCESS_OR_DIE (gpi_cp_commit, checkpoint_description, GASPI_BLOCK);

	      /* Save data to be checkpointed */
	      memcpy(checkpoint_seg_ptr, buffer[from], size_global_x * (size_local_y + 2 * stencil_height()) * sizeof (element_type));

	      /* Start a new checkpoint */
	      SUCCESS_OR_DIE ( gpi_cp_start, checkpoint_description, GASPI_BLOCK ) ;
	    }
#endif

	  /* send border data into halo area of neighbours */
	  wait_for_queue_entries (&queue, 4);

	  SUCCESS_OR_DIE
	    ( gaspi_write_notify
	      , segment_id[from]
	      , size_global_x * stencil_height() * sizeof (element_type)
	      , iAbove
	      , segment_id[from]
	      , size_global_x * (size_local_y_above + stencil_height()) * sizeof (element_type)
	      , size_global_x * stencil_height() * sizeof (element_type)
	      , flag_from_below
	      , (gaspi_notification_t) (1 + k)
	      , queue
	      , GASPI_BLOCK
	      );

	  SUCCESS_OR_DIE
	    ( gaspi_write_notify
	      , segment_id[from]
	      , size_global_x * size_local_y * sizeof (element_type)
	      , iBelow
	      , segment_id[from]
	      , 0
	      , size_global_x * stencil_height() * sizeof (element_type)
	      , flag_from_above
	      , (gaspi_notification_t) (1 + k)
	      , queue
	      , GASPI_BLOCK
	      );

	  /* compute inner */
	  STEP (2 * stencil_height(), size_local_y);

	  /* wait for neighbor data */
	  int missing = 2;

	  while (missing --> 0)
	    {
	      gaspi_notification_id_t id;

	      SUCCESS_OR_DIE
		(gaspi_notify_waitsome, segment_id[from], 0, 2, &id, GASPI_BLOCK);

	      gaspi_notification_t value;

	      SUCCESS_OR_DIE (gaspi_notify_reset, segment_id[from], id, &value);

	      /* work on data while data from other neighbors might still in flight */
	      if (id == flag_from_above)
		{
		  STEP (stencil_height(), 2 * stencil_height());
		}
	      else if (id == flag_from_below)
		{
		  STEP (size_local_y, size_local_y + stencil_height());
		}
	      else
		{
		  ERROR ("strange notification id");
		}
	    }
        }
    }

  /* compute and print the computation time in millisec */
  gettimeofday(&tcompute_end, NULL);
  compute_time = 
    ((tcompute_end.tv_sec - tcompute_start.tv_sec) * 1000.0) 
    + ((tcompute_end.tv_usec - tcompute_start.tv_usec) / 1000.0);

  gaspi_printf("Computation time: %.2f ms\n", compute_time);

  if(rank_is_active)
    {
      double norm1 = euclidean_norm(group_active, data[(iteration - 1) % 2], size_global_x * size_local_y);
      double norm2 = euclidean_norm(group_active, data[1- ((iteration - 1) % 2)], size_global_x * size_local_y);
      gaspi_printf("Norm %.2f %.2f\n", norm1, norm2);
    }

  for (int i = 0; i < 2; ++i)
    {
      SUCCESS_OR_DIE (gaspi_segment_delete, segment_id[i]);
    }

  gettimeofday(&ttotal_end, NULL);
  total_time = ((ttotal_end.tv_sec - ttotal_start.tv_sec) * 1000.0) + ((ttotal_end.tv_usec - ttotal_start.tv_usec) / 1000.0);

  if(rank_is_active)
    {
      double max_tot_time, max_comp_time;

      SUCCESS_OR_DIE (gaspi_allreduce
		      , &total_time
		      , &max_tot_time
		      , 1
		      , GASPI_OP_MAX
		      , GASPI_TYPE_DOUBLE
		      , group_active
		      , GASPI_BLOCK);

      SUCCESS_OR_DIE (gaspi_allreduce
		      , &compute_time
		      , &max_comp_time
		      , 1
		      , GASPI_OP_MAX
		      , GASPI_TYPE_DOUBLE
		      , group_active
		      , GASPI_BLOCK);
      if(iProc == 0)
	printf("Max total time: %.2f ms - Computation %.2f ms\n",
	       max_tot_time, max_comp_time);

#ifdef WITH_CHECKPOINT
      SUCCESS_OR_DIE(gpi_cp_finalize, checkpoint_description, GASPI_BLOCK);
#endif
    }
  SUCCESS_OR_DIE (gaspi_proc_term, GASPI_BLOCK);

  return EXIT_SUCCESS;
}
