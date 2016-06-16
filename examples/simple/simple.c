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
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define WITH_CHECKPOINT 1 /*  1: every nth iteration */
                          /*  2: high-pressure and more synchronous */

#define SUCCESS_OR_DIE(f...)			\
  do						\
    {						\
      gaspi_return_t const r = f;				\
      if (r != GASPI_SUCCESS)					\
	{							\
	  fprintf(stderr, "%s\n", gaspi_error_str (r));		\
	}							\
    } while (0)

gaspi_return_t
g_create_group(gaspi_rank_t nprocs, gaspi_group_t *g, gaspi_rank_t avoid)
{
  gaspi_rank_t myrank;
  gaspi_number_t gsize;

  SUCCESS_OR_DIE(gaspi_proc_rank(&myrank));
  
  SUCCESS_OR_DIE (gaspi_group_create(g));

  gaspi_printf("Created group %d\n", *g);
  
  SUCCESS_OR_DIE(gaspi_group_size(*g, &gsize));
  
  gaspi_rank_t i;
  for(i = 0; i < nprocs; i++)
    {
      if(i != avoid)
	SUCCESS_OR_DIE(gaspi_group_add(*g, i));
    }

  SUCCESS_OR_DIE(gaspi_group_size(*g, &gsize));
 
  SUCCESS_OR_DIE(gaspi_group_commit(*g, GASPI_BLOCK));

  gaspi_printf("Finished group %d\n", *g);

  return GASPI_SUCCESS;
}

/*  simple example application to understand usage of checkpoints */
int
main(void)
{
  int i, j;
  const int iterations = 1000;
  gaspi_rank_t myrank, nranks;
  
  SUCCESS_OR_DIE (gaspi_proc_init (GASPI_BLOCK));

  gaspi_size_t const size = ( 1 << 21);
  SUCCESS_OR_DIE(gaspi_proc_rank(&myrank));
  SUCCESS_OR_DIE(gaspi_proc_num(&nranks));

  gaspi_segment_id_t segment_id_work = 1;
  gaspi_pointer_t work_seg_ptr;
  int * work_array;
  int num_work_elems = size / sizeof(int) ;

  /* Create segment for work */
  SUCCESS_OR_DIE (gaspi_segment_create (segment_id_work, size, GASPI_GROUP_ALL,
					GASPI_BLOCK, GASPI_MEM_UNINITIALIZED));

  gaspi_segment_ptr(segment_id_work, &work_seg_ptr);
  work_array = (int *) work_seg_ptr;

#ifdef WITH_CHECKPOINT
  gaspi_segment_id_t segment_id_checkpoint = 0;
  gaspi_pointer_t checkpoint_seg_ptr;
  gaspi_group_t g, new_group;
  gaspi_rank_t spare = nranks - 1;
  const gaspi_rank_t culprit = nranks - 2;

  if(myrank != spare)
    SUCCESS_OR_DIE(g_create_group(nranks, &g, spare));
  
  /* All create segment to be checkpointed */
  SUCCESS_OR_DIE (gaspi_segment_create (segment_id_checkpoint, size, GASPI_GROUP_ALL,
					GASPI_BLOCK, GASPI_MEM_UNINITIALIZED));
  
  gaspi_segment_ptr(segment_id_checkpoint, &checkpoint_seg_ptr);
  
  gpi_cp_description_t
    checkpoint_description = GPI_CP_DESCRIPTION_INITIALIZER();

  if( myrank != spare )
    {
      SUCCESS_OR_DIE ( gpi_cp_init ( segment_id_checkpoint
					 , 0
					 , size
					 , 4
					 , GPI_CP_POLICY_RING
					 , g
					 , checkpoint_description
                                         , GASPI_BLOCK
				 )
		       );
    }
#endif
  
  for (i = 0; i < iterations; i++ )
    {
#ifdef WITH_CHECKPOINT
      if( myrank != spare )
	{
	  /* If it's time to checkpoint */
#if WITH_CHECKPOINT == 1

	  /* VARIANT 1: every nth iteration */
	  if ( (i % 100 ) == 0 )
	    {
	      /* Commit previously started checkpoint */
	      SUCCESS_OR_DIE (gpi_cp_commit (checkpoint_description, GASPI_BLOCK));
	      
	      /* Save data to be checkpointed */
	      memcpy(checkpoint_seg_ptr, work_seg_ptr, size);
	      
	      /* Start a new checkpoint */
	      SUCCESS_OR_DIE ( gpi_cp_start (checkpoint_description, GASPI_BLOCK) );
	    }
#elif WITH_CHECKPOINT == 2
	  /* VARIANT 2: high-pressure and more synchronous */
	  if( !gpi_cp_get_state_in_progress(checkpoint_description))
	    {
	      /* Save data to be checkpointed */
	      memcpy(checkpoint_seg_ptr, work_seg_ptr, size);
	      
	      /* Start a new checkpoint */
	      SUCCESS_OR_DIE (gpi_cp_start(checkpoint_description, GASPI_BLOCK));
	    }
	  else
	    {
	      /* Commit previously started checkpoint */
	      SUCCESS_OR_DIE (gpi_cp_commit(checkpoint_description, GASPI_BLOCK));
	    }
#endif
	}
#endif
      /* do some useful work */
      for( j = 0; j < num_work_elems; j++ )
	{
	  work_array[j] = i;
	}

#ifdef WITH_CHECKPOINT
      /* Simulate a fault */
      if ( i == 666 )
	{
	  gaspi_printf("FAULT!!!\n");

	  if(myrank != spare )
	    {
	      /* Delete old group */
	      SUCCESS_OR_DIE(gaspi_group_delete(g));
	    }

	  /* Update */
	  spare = culprit;

	  if(myrank != culprit )
	    {
	      /* Create the new group */
	      SUCCESS_OR_DIE(g_create_group(nranks, &new_group, culprit));
	    }
	  
	  if( myrank == culprit )
	    {
	      /* I'm failing, jump out */
	      //	      break;
	      _exit(-1);
	    }
	  
	  /* Do Restore */
	  SUCCESS_OR_DIE(gpi_cp_restore( segment_id_checkpoint
					     , 0
					     , size
					     , 4
					     , GPI_CP_POLICY_RING
					     , new_group
					     , checkpoint_description
                                             , GASPI_BLOCK
			     )
			 );
	}
#endif
    }


#ifdef WITH_CHECKPOINT
  if( myrank != culprit )
    {
      /* Finalize with a checkpoint in progress = undefined */
      SUCCESS_OR_DIE (gpi_cp_commit (checkpoint_description, GASPI_BLOCK));
      
      SUCCESS_OR_DIE(gaspi_barrier(new_group, GASPI_BLOCK));
      
      /* Check data */
      SUCCESS_OR_DIE( gpi_cp_read_buddy(checkpoint_description, GASPI_BLOCK));
      
      {
	int * int_receiver_seg = (int *) gpi_cp_get_receiver_ptr(checkpoint_description);
	int *int_sender_seg = (int *) ((char*) int_receiver_seg + gpi_cp_get_active_snapshot(checkpoint_description));

	for( j = 0; j < num_work_elems; j++ )
	  {
	    /* Check receiver data */
	    if( int_receiver_seg[j] != ((int *) checkpoint_seg_ptr)[j])
	      {
		gaspi_printf("Different receiver data in pos %i %i %i\n",
			     j,
			     int_receiver_seg[j], ((int *) checkpoint_seg_ptr)[j]);
	      }
	    
	    /* Check sender data */
	    if( int_sender_seg[j] != ((int *) checkpoint_seg_ptr)[j])
	      {
		gaspi_printf("Different sender data in pos %i %i %i\n",
			     j,
			     int_sender_seg[j], ((int *) checkpoint_seg_ptr)[j]);
	      }
	  }
      }
      
      SUCCESS_OR_DIE(gaspi_barrier(new_group, GASPI_BLOCK));
      SUCCESS_OR_DIE (gpi_cp_finalize ( checkpoint_description, GASPI_BLOCK ));
    }
#endif /* WITH_CHECKPOINT */
  
  SUCCESS_OR_DIE( gaspi_proc_term(GASPI_BLOCK));

  return 0;
}


