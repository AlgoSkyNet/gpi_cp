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


#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include <GASPI.h>
#include <gpi_cp.h>

#define CP_STATS 1

#ifdef CP_STATS
#include <sys/time.h>
#endif

#define GPI_CP_MAJOR_VERSION (1)
#define GPI_CP_MINOR_VERSION (0)

#define GPI_CP_VERSION (GPI_CP_MAJOR_VERSION + GPI_CP_MINOR_VERSION/10.0f)

/* #define NDEBUG 1 */

#ifndef NDEBUG
#define DEBUG_PRINT(...) gaspi_printf (__VA_ARGS__);
#else
#define DEBUG_PRINT(...)
#endif

#define MAX(a,b) (((a)>(b))?(a):(b))

#define GASPI_SUCCESS_OR_RETURN(f...)                          \
  do                                                           \
    {                                                          \
      const gaspi_return_t r = f;                              \
                                                               \
      if (r != GASPI_SUCCESS)                                  \
       {                                                       \
         return r;                                             \
       }                                                       \
    } while (0)

#define CP_SUCCESS_OR_RETURN(f...)                             \
  do                                                           \
    {                                                          \
      const gpi_cp_error_codes r = f;                          \
                                                               \
      if (r != GPI_CP_SUCCESS)                                 \
       {                                                       \
         return GASPI_ERROR;                                   \
       }                                                       \
    } while (0)

struct gpi_cp_description
{
  gaspi_offset_t offset;
  gaspi_size_t size;
  gaspi_segment_id_t segment_id_local_client_source;
  gaspi_queue_id_t queue;
  gaspi_group_t group;

  gaspi_rank_t sender;
  gaspi_segment_id_t segment_id_local_for_sender;

  gaspi_rank_t receiver;
  gaspi_segment_id_t segment_id_remote_on_receiver;

  gaspi_offset_t active_snapshot; // toggles between 0 and size
  bool state_in_progress;
  bool state_initialized;

#ifdef CP_STATS
  /* Timings for benchmarking */
  struct timeval in_init;
  struct timeval in_start;
  struct timeval in_commit;
  struct timeval in_restore;
#endif
};

void gpi_cp_description_print(gpi_cp_description_t description)
{
  gaspi_printf("description print: offset %i, size %i, segment_id_local_client_source %i, queue %i, group %i, sender %i, segment_id_local_for_sender %i, receiver %i, segment_id_remote_on_receiver %i, active_snapshot %i, size %lu, state_in_progress %i, state_initialized %i\n",
              description->offset,
              description->size,
              description->segment_id_local_client_source,
              description->queue,
              description->group,
              description->sender,
              description->segment_id_local_for_sender,
              description->receiver,
              description->segment_id_remote_on_receiver,
              description->active_snapshot,
              description->size,
              description->state_in_progress,
              description->state_initialized);
} 

gpi_cp_description_t GPI_CP_DESCRIPTION_INITIALIZER()
{
  gpi_cp_description_t description = malloc( sizeof ( struct gpi_cp_description));
  if (description != NULL)
    {
      description->state_in_progress = false;
      description->state_initialized = false;
#ifdef CP_STATS
      memset(&(description->in_init), 0, sizeof(description->in_init));
      memset(&(description->in_start), 0, sizeof(description->in_start));
      memset(&(description->in_commit), 0, sizeof(description->in_commit));
      memset(&(description->in_restore), 0, sizeof(description->in_restore));
#endif
    }

  return description;
}

gaspi_return_t
gpi_cp_version (float *const version)
{
 if (version == NULL)
     return GASPI_ERROR;

  *version = GPI_CP_VERSION;
  return GASPI_SUCCESS;
}

gaspi_return_t
gpi_cp_get_unused_segment_id (gaspi_segment_id_t* unused_segment_id)
{
  gaspi_number_t number_of_allocated_segments;
  GASPI_SUCCESS_OR_RETURN (gaspi_segment_num (&number_of_allocated_segments));
  DEBUG_PRINT("number of allocated segments: %i\n", number_of_allocated_segments);

  if (number_of_allocated_segments == 0)
    {
      DEBUG_PRINT("No allocated segments yet\n");
      *unused_segment_id = 0;
      DEBUG_PRINT("Unused segment id: %i\n", *unused_segment_id);
      return GASPI_SUCCESS;
    }

  gaspi_segment_id_t segment_ids[number_of_allocated_segments];
  GASPI_SUCCESS_OR_RETURN(gaspi_segment_list (number_of_allocated_segments, segment_ids));

  gaspi_number_t segment_max;
  GASPI_SUCCESS_OR_RETURN (gaspi_segment_max (&segment_max));

  gaspi_number_t i;
  for(i = 0; i < number_of_allocated_segments-1; ++i)
    {
      if(segment_ids[i]+1 != segment_ids[i+1])
       {
         *unused_segment_id=segment_ids[i]+1;
         return GASPI_SUCCESS;
       }
    }

  *unused_segment_id = segment_ids[number_of_allocated_segments-1] + 1;

  DEBUG_PRINT("Unused segment id: %i\n", *unused_segment_id);

  return GASPI_SUCCESS;
}

static bool
gpi_cp_is_in_group ( gaspi_group_t group
                   , gaspi_rank_t rank)
{
  gaspi_number_t size;

  if ( GASPI_SUCCESS != gaspi_group_size (group, &size) )
    return false;

  gaspi_rank_t ranks[size];

  if ( GASPI_SUCCESS != gaspi_group_ranks (group, ranks) )
    return false;

  gaspi_number_t i;
  for (i = 0; i < size; ++i)
  {
    if (ranks[i] == rank)
    {
      return true;
    }
  }

  return false;
}

static gpi_cp_error_codes
gpi_cp_sender ( gpi_cp_policy_t policy
              , gaspi_group_t group
              , gaspi_rank_t rank
              , gaspi_rank_t * const sender
              )
{
  gpi_cp_error_codes cp_ret = GPI_CP_SUCCESS;
  switch (policy)
    {
    case GPI_CP_POLICY_RING:
      {
       gaspi_rank_t nProc;
       if ( GASPI_SUCCESS == gaspi_proc_num( &nProc) )
         {
           *sender = (rank + nProc - 1) % nProc;

           while ( !gpi_cp_is_in_group (group, *sender) )
             {
              *sender = (*sender + nProc - 1) % nProc;
             }

           DEBUG_PRINT ("Setting sender %i from rank %i\n", *sender, rank);
         }
       else
         {
           DEBUG_PRINT ("Could not set sender for rank %i\n", rank);
           *sender = rank;
           cp_ret = GPI_CP_ERROR_UNDEFINED_RANK;
         }
       break;
      }
    default:
      fprintf (stderr, "Unknown checkpointing policy\n");
      *sender = rank;
      cp_ret = GPI_CP_ERROR_UNDEFINED_RANK;
    }
  return cp_ret;
}

static gpi_cp_error_codes
gpi_cp_receiver ( gpi_cp_policy_t policy
                , gaspi_group_t group
                , gaspi_rank_t rank
                , gaspi_rank_t * const receiver
                )
{
  gpi_cp_error_codes cp_ret = GPI_CP_SUCCESS;
  switch (policy)
    {
    case GPI_CP_POLICY_RING:
      {
       gaspi_rank_t nProc;
       if ( GASPI_SUCCESS == gaspi_proc_num(&nProc) )
         {
           *receiver = (rank + 1) % nProc;

           while ( !gpi_cp_is_in_group (group, *receiver) )
             {
              *receiver = (*receiver + 1) % nProc;
             }
           DEBUG_PRINT ("Setting receiver %i from rank %i group size %i\n", *receiver, rank, nProc);
         }
       else
         {
           DEBUG_PRINT ("Could not set receiver for rank %i\n", rank);
           *receiver = rank;
           cp_ret = GPI_CP_ERROR_UNDEFINED_RANK;
         }
       break;
      }
    default:
      fprintf (stderr, "Unknown checkpointing policy\n");
      *receiver = rank;
      cp_ret = GPI_CP_ERROR_UNDEFINED_RANK;
    }
  return cp_ret;
}

static gaspi_segment_id_t*
gpi_cp_ptr ( gaspi_segment_id_t segment_id
           , gaspi_offset_t offset)
{
  gaspi_pointer_t pointer_segment;
  
  if( GASPI_SUCCESS ==  (gaspi_segment_ptr (segment_id, &pointer_segment)) )
    return (gaspi_segment_id_t*) ((char*) pointer_segment + offset);

  return NULL;
}

static void
gpi_cp_tell_sender_about_the_local_segment_id ( gaspi_segment_id_t segment_id_local_for_sender
                                              , gaspi_offset_t offset
                                              , gaspi_rank_t sender
                                              )
{
  gpi_cp_ptr (segment_id_local_for_sender, offset)[0] = segment_id_local_for_sender;

  /* We have to send in a non-blocking way => GASPI_TEST */
  gaspi_return_t ret = gaspi_passive_send ( segment_id_local_for_sender
                                       , offset
                                       , sender
                                       , sizeof (gaspi_segment_id_t)
                                       , GASPI_TEST
                                       );

  if(ret != GASPI_TIMEOUT && ret != GASPI_SUCCESS)
    {
      gaspi_printf("Failed to tell sender (error %s)\n", gaspi_error_str(ret));
    }
}

static void
gpi_cp_make_sure_receiver_got_local_segment_id ( const gaspi_segment_id_t segment_id_local_for_sender
                                               , const gaspi_offset_t offset
                                               , const gaspi_rank_t sender
                                               , const gaspi_timeout_t timeout_ms
                                               )
{
  gpi_cp_ptr (segment_id_local_for_sender, offset)[0] = segment_id_local_for_sender;

  gaspi_pointer_t pointer_segment;
  if ( GASPI_SUCCESS != gaspi_segment_ptr (segment_id_local_for_sender, &pointer_segment))
    return;
  
  if( GASPI_SUCCESS != (gaspi_passive_send ( segment_id_local_for_sender
                                        , offset
                                        , sender
                                        , sizeof (gaspi_segment_id_t)
                                        , timeout_ms
                                        )
                     ) )
    return;
}
  
static gpi_cp_error_codes
gpi_cp_receive_segment_id ( const gaspi_segment_id_t segment_id_local_for_sender
                          , const gaspi_offset_t offset
                          , const gaspi_rank_t expected_notifier
                          , const gaspi_timeout_t timeout_ms
                          , gaspi_segment_id_t * const segment_id_received
                          )
{
  gpi_cp_error_codes cp_ret = GPI_CP_SUCCESS;
  *segment_id_received = 33;  // Invalid value
  gaspi_rank_t notifier;
  if ( GASPI_SUCCESS  != gaspi_passive_receive ( segment_id_local_for_sender
                                           , offset + sizeof (gaspi_segment_id_t)
                                           , &notifier
                                           , sizeof (gaspi_segment_id_t)
                                           , timeout_ms
                                           ) )
  {
    cp_ret = GPI_CP_ERROR_UNEXPECTED_SEGMENT_ID_SOURCE;
    return cp_ret;
  }

  if (notifier != expected_notifier)
    {
      gaspi_printf ("BUMMER: Got segment_id from unexpected source (%u %u)\n",  notifier, expected_notifier);
      cp_ret = GPI_CP_ERROR_UNEXPECTED_SEGMENT_ID_SOURCE;
      return cp_ret;
    }
  *segment_id_received = gpi_cp_ptr (segment_id_local_for_sender, offset)[1];
  return cp_ret;
}


static void
gpi_cp_allocate_and_register_local_segment ( gaspi_segment_id_t *segment_id_local_for_sender
                                           , const gaspi_size_t size
                                           , const gaspi_rank_t sender
                                           , const gaspi_timeout_t timeout_ms
                                           )
{
  gaspi_size_t const number_of_snapshots = 2;

  if ( GASPI_SUCCESS != gpi_cp_get_unused_segment_id (segment_id_local_for_sender) ) 
    return;
  
  if ( GASPI_SUCCESS != gaspi_segment_alloc ( *segment_id_local_for_sender
                                         , MAX ( number_of_snapshots * size, number_of_snapshots * (2 * sizeof (gaspi_segment_id_t)))
                                         , GASPI_MEM_UNINITIALIZED
                                         ) )
    return;
  
  if ( GASPI_SUCCESS != gaspi_segment_register ( *segment_id_local_for_sender
                                           , sender
                                           , timeout_ms
                                           ) )
    return;

  return;
}
static inline double
gpi_cp_timeval_to_ms(struct timeval t)
{
  return (t.tv_sec * 1000.0) + ((t.tv_usec) / 1000.0);
}


gaspi_return_t
gpi_cp_finalize ( const gpi_cp_description_t description
                , const gaspi_timeout_t timeout_ms
                )
{
  /*   if( !description->state_initialized) */
  /*     return GASPI_ERROR; */

  /*   if( description->state_in_progress) */
  /*     return GASPI_ERROR; */
  gaspi_rank_t iProc;
  GASPI_SUCCESS_OR_RETURN (gaspi_proc_rank (&iProc));

  if(gpi_cp_is_in_group(description->group, iProc))
    {
      GASPI_SUCCESS_OR_RETURN (gaspi_segment_delete (description->segment_id_local_for_sender));

#ifdef CP_STATS
      double max_total[5] ={ 0.0f };
      double total[5];

      total[1] = gpi_cp_timeval_to_ms(description->in_start);
      total[2] = gpi_cp_timeval_to_ms(description->in_init);
      total[3] = gpi_cp_timeval_to_ms(description->in_commit);
      total[4] = gpi_cp_timeval_to_ms(description->in_restore);
      total[0] = total[1] + total[2] + total[3] + total[4];
  
      gaspi_printf("CP Stats (in ms): start %.4f init %.4f commit %.4f restore %.4f total %.4f\n",
                 gpi_cp_timeval_to_ms(description->in_start),
                 gpi_cp_timeval_to_ms(description->in_init),
                 gpi_cp_timeval_to_ms(description->in_commit),
                 gpi_cp_timeval_to_ms(description->in_restore),
                 total[0]);

      gaspi_allreduce(&total
                    , &max_total
                    , 5
                    , GASPI_OP_MAX
                    , GASPI_TYPE_DOUBLE
                    , description->group
                    , timeout_ms);

      gaspi_rank_t myrank;
      GASPI_SUCCESS_OR_RETURN (gaspi_proc_rank (&myrank));
      if( 0 == myrank )
       printf("Max CP times: total %.4f, start  %.4f init  %.4f commit  %.4f restore %.4f \n",
              max_total[0],
              max_total[1],
              max_total[2],
              max_total[3],
              max_total[4]);
#endif
    }
  return GASPI_SUCCESS;
}

gaspi_return_t
gpi_cp_init ( const gaspi_segment_id_t segment_id_checkpoint
            , const gaspi_offset_t offset
            , const gaspi_size_t size
            , const gaspi_queue_id_t queue
            , const gpi_cp_policy_t policy
            , const gaspi_group_t group
            , gpi_cp_description_t description
            , const gaspi_timeout_t timeout_ms
            )
{
#ifdef CP_STATS
  struct timeval tstart, tend;
  gettimeofday(&tstart, NULL);
#endif

  description->offset = offset;
  description->size = size;
  description->segment_id_local_client_source = segment_id_checkpoint;
  description->queue = queue;
  description->group = group;
  description->active_snapshot = 0;
  
  gaspi_rank_t iProc;
  GASPI_SUCCESS_OR_RETURN (gaspi_proc_rank (&iProc));

  if(gpi_cp_is_in_group(description->group, iProc))
    {
      CP_SUCCESS_OR_RETURN( gpi_cp_sender (policy, group, iProc, &(description->sender)) );
      CP_SUCCESS_OR_RETURN( gpi_cp_receiver (policy, group, iProc, &(description->receiver)) );

      gpi_cp_allocate_and_register_local_segment
       ( &description->segment_id_local_for_sender
         , description->size
         , description->sender
         , timeout_ms
        );
      
      gpi_cp_tell_sender_about_the_local_segment_id
       ( description->segment_id_local_for_sender
         , description->active_snapshot
         , description->sender
         );

      CP_SUCCESS_OR_RETURN( gpi_cp_receive_segment_id ( description->segment_id_local_for_sender
                                                      , description->active_snapshot
                                                      , description->receiver
                                                      , timeout_ms
                                                      , &(description->segment_id_remote_on_receiver))
           );

      gpi_cp_make_sure_receiver_got_local_segment_id
       ( description->segment_id_local_for_sender
         , description->active_snapshot
         , description->sender
         , timeout_ms
         );
       
      description->state_initialized = true;
    }
/*       description_print(description); */

  //! \todo ensure there are at least nProc many notification ids available
#ifdef CP_STATS
  gettimeofday(&tend, NULL);
  description->in_init.tv_usec += (tend.tv_usec - tstart.tv_usec);
  description->in_init.tv_sec += (tend.tv_sec - tstart.tv_sec);
#endif

  return GASPI_SUCCESS;
}


gaspi_return_t
gpi_cp_start ( gpi_cp_description_t description
             , const gaspi_timeout_t timeout_ms
             )
{
#ifdef CP_STATS
  struct timeval tstart, tend;
  gettimeofday(&tstart, NULL);
#endif

  gaspi_rank_t iProc;
  GASPI_SUCCESS_OR_RETURN (gaspi_proc_rank (&iProc));

  if(gpi_cp_is_in_group(description->group, iProc))
    {
      if (description->state_in_progress)
       {
         return GASPI_ERROR; //! \todo specific error code
       }

      description->state_in_progress = true;

/*       description_print(description); */
      DEBUG_PRINT("gpi_cp_start: gaspi_write_notify(%i, %i, %i, %i, %i, %i, %i, %i, %i)\n",
                 description->segment_id_local_client_source , description->offset, description->receiver,
                 description->segment_id_remote_on_receiver, description->active_snapshot, description->size,
                 (gaspi_notification_id_t) iProc, iProc + 1,
                 description->queue);

      gaspi_number_t queueSize, qmax;
      GASPI_SUCCESS_OR_RETURN (gaspi_queue_size_max(&qmax));
      gaspi_queue_size(description->queue, &queueSize);
      if (queueSize > qmax - 24)
       GASPI_SUCCESS_OR_RETURN (gaspi_wait(description->queue, timeout_ms));
      
      GASPI_SUCCESS_OR_RETURN
       (gaspi_write_notify (description->segment_id_local_client_source // segment_id_local
                          , description->offset // offset_local
                          , description->receiver // rank
                          , description->segment_id_remote_on_receiver
                          , description->active_snapshot // offset_remote
                          , description->size // size
                          , (gaspi_notification_id_t) iProc // notification_id
                          , (gaspi_notification_t) iProc+1 // notification_value
                          , description->queue // queue
                          , timeout_ms
                          )
        );
    }

#ifdef CP_STATS
  gettimeofday(&tend, NULL);
  description->in_start.tv_usec += (tend.tv_usec - tstart.tv_usec);
  description->in_start.tv_sec += (tend.tv_sec - tstart.tv_sec);
#endif

  return GASPI_SUCCESS;
}


static gaspi_return_t
gpi_cp_wait_for_notification_from ( const gaspi_segment_id_t segment_id_local_for_sender
                                  , const gaspi_rank_t sender
                                  , const gaspi_notification_t expected_value
                                  , const gaspi_timeout_t timeout_ms
                                  )
{
  gaspi_notification_id_t notifier;
  GASPI_SUCCESS_OR_RETURN ( gaspi_notify_waitsome
                      ( segment_id_local_for_sender
                        , (gaspi_notification_id_t) sender 
                        , (gaspi_number_t) 1
                        , &notifier
                        , timeout_ms
                        )
                      );

  if (notifier != sender)
  {
    fprintf (stderr, "Unexpected notification\n");
    return GASPI_ERROR; //! \todo specific error code
  }
  
  gaspi_notification_t value;
  GASPI_SUCCESS_OR_RETURN( gaspi_notify_reset (segment_id_local_for_sender, notifier, &value) );

  if (value != expected_value)
  {
    fprintf (stderr, "Wrong notification value: %i, %i \n", value, expected_value);
    return GASPI_ERROR; //! \todo specific error code
  }

  return GASPI_SUCCESS; //! \todo specific error code
}

gaspi_return_t
gpi_cp_commit ( gpi_cp_description_t description
              , const gaspi_timeout_t timeout_ms )
{
#ifdef CP_STATS
  struct timeval tstart, tend;
  gettimeofday(&tstart, NULL);
#endif

  gaspi_rank_t iProc;
  GASPI_SUCCESS_OR_RETURN (gaspi_proc_rank (&iProc));
  
  if(gpi_cp_is_in_group(description->group, iProc))
    {
      if (description->state_in_progress)
       {
         GASPI_SUCCESS_OR_RETURN (gaspi_wait (description->queue, timeout_ms));
         
         GASPI_SUCCESS_OR_RETURN( gpi_cp_wait_for_notification_from ( description->segment_id_remote_on_receiver
                                                      , description->sender
                                                      , (description->sender)+1
                                                      , timeout_ms
                                                     )
                            );
         GASPI_SUCCESS_OR_RETURN (gaspi_barrier (description->group, timeout_ms));
         
         // make persistent copies here!
         
         description->active_snapshot = description->size - description->active_snapshot;
         description->state_in_progress = false;
       }
    }
#ifdef CP_STATS
  gettimeofday(&tend, NULL);
  description->in_commit.tv_usec += (tend.tv_usec - tstart.tv_usec);
  description->in_commit.tv_sec += (tend.tv_sec - tstart.tv_sec);
#endif

  return GASPI_SUCCESS;
}

gaspi_return_t
gpi_cp_restore ( const gaspi_segment_id_t segment_id_checkpoint
               , const gaspi_offset_t offset
               , const gaspi_size_t size
               , const gaspi_queue_id_t queue
               , const gpi_cp_policy_t policy
               , const gaspi_group_t new_group
               , gpi_cp_description_t description
               , const gaspi_timeout_t timeout_ms
               )
{
#ifdef CP_STATS
  struct timeval tstart, tend;
  gettimeofday(&tstart, NULL);
#endif

  description->offset = offset;
  description->size = size;
  description->segment_id_local_client_source = segment_id_checkpoint;
  description->queue = queue;
  description->group = new_group;

  gaspi_rank_t iProc;
  GASPI_SUCCESS_OR_RETURN (gaspi_proc_rank (&iProc));

  // case joiner
  if (!description->state_initialized)
    {
      CP_SUCCESS_OR_RETURN( gpi_cp_sender (policy, new_group, iProc, &(description->sender)) );
      CP_SUCCESS_OR_RETURN( gpi_cp_receiver (policy, new_group, iProc, &(description->receiver)) );
      description->state_initialized = true;

      // in case of two consecutive joiners: one needs to go into a send!?
      {
       gaspi_rank_t notifier;

       GASPI_SUCCESS_OR_RETURN
         ( gaspi_passive_receive ( segment_id_checkpoint
                                , offset
                                , &notifier
                                , 1
                                , timeout_ms
                                )
           );

       if (notifier == description->sender)
         {
           description->active_snapshot = 0;
         }
       else if (notifier == description->receiver)
         {
           description->active_snapshot = size;
         }
       else
         {
           fprintf (stderr, "BUMMER: Got message from unexpected source\n");
           return GASPI_ERROR;  //! \todo specific error code
         }
      }
      gaspi_barrier(description->group, timeout_ms);

      gpi_cp_allocate_and_register_local_segment
       ( &description->segment_id_local_for_sender
         , description->size
         , description->sender
         , timeout_ms
         );
      
      gpi_cp_tell_sender_about_the_local_segment_id
       ( description->segment_id_local_for_sender
         , description->active_snapshot
         , description->sender
         );

      CP_SUCCESS_OR_RETURN( gpi_cp_receive_segment_id ( description->segment_id_local_for_sender
                                                       , description->active_snapshot
                                                       , description->receiver
                                                       , timeout_ms
                                                       , &(description->segment_id_remote_on_receiver))
          );

      gpi_cp_make_sure_receiver_got_local_segment_id
       ( description->segment_id_local_for_sender
         , description->active_snapshot
         , description->sender
         , timeout_ms
         );

      GASPI_SUCCESS_OR_RETURN
       ( gaspi_read
         ( description->segment_id_local_client_source
           , description->offset
           , description->receiver
           , description->segment_id_remote_on_receiver
           , (gaspi_offset_t) (description->size - description->active_snapshot)
           , description->size
           , description->queue
           , timeout_ms
           )
         );
      
      gpi_cp_wait_for_notification_from
       ( description->segment_id_local_for_sender
         , description->sender
         , (description->sender)+1
         , timeout_ms
         );

      GASPI_SUCCESS_OR_RETURN (gaspi_wait (description->queue, timeout_ms));
    }

  // case affected_missing_sender
  else if (!gpi_cp_is_in_group (new_group, description->sender))
    {
      CP_SUCCESS_OR_RETURN( gpi_cp_sender (policy, new_group, iProc, &(description->sender)) );

      if (description->active_snapshot == description->size)
       {
         // V.B.: hope I have connected the correct segment
         GASPI_SUCCESS_OR_RETURN
           ( gaspi_passive_send ( description->segment_id_local_for_sender
                               , description->active_snapshot
                               , description->sender
                               , 1
                               , timeout_ms
                               )
             );
       }
      gaspi_barrier(description->group, timeout_ms);

      GASPI_SUCCESS_OR_RETURN
       ( gaspi_segment_register
         ( description->segment_id_local_for_sender
           , description->sender
           , timeout_ms
           )
         );
      
      gpi_cp_tell_sender_about_the_local_segment_id
       ( description->segment_id_local_for_sender
         , description->active_snapshot
         , description->sender
         );
    }

  // case affected_missing_receiver
  else if (!gpi_cp_is_in_group (new_group, description->receiver))
    {
      CP_SUCCESS_OR_RETURN( gpi_cp_receiver (policy, new_group, iProc, &(description->receiver)) );

      if (description->active_snapshot == 0)
       {
         GASPI_SUCCESS_OR_RETURN
           (gaspi_passive_send ( description->segment_id_local_for_sender
                              , description->active_snapshot
                              , description->receiver
                              , 1
                              , timeout_ms
                              )
            );
       }
      gaspi_barrier(description->group, timeout_ms);

      CP_SUCCESS_OR_RETURN( gpi_cp_receive_segment_id ( description->segment_id_local_for_sender
                                                      , description->active_snapshot
                                                      , description->receiver
                                                      , timeout_ms
                                                      , &(description->segment_id_remote_on_receiver))
          );

      if (description->state_in_progress)
       {
         GASPI_SUCCESS_OR_RETURN (gaspi_wait (description->queue, timeout_ms));
         description->state_in_progress = false;
       }

      gpi_cp_start ( description
                   , timeout_ms);

      GASPI_SUCCESS_OR_RETURN ( gaspi_wait (description->queue, timeout_ms));
    }

  // case unaffected
  else
    {
      gaspi_printf("Unaffected\n");
      assert (gpi_cp_is_in_group (new_group, description->receiver));
      assert (gpi_cp_is_in_group (new_group, description->sender));

      // do nothing: the data still resides in local memory
      gaspi_barrier(description->group, timeout_ms);
    }

  //! \todo is this correct?
  description->state_in_progress = false;

  //! \todo required!? -> maybe yes to allow immediate checkpoint_start
  GASPI_SUCCESS_OR_RETURN (gaspi_barrier (description->group, timeout_ms));

#ifdef CP_STATS
  gettimeofday(&tend, NULL);
  description->in_restore.tv_usec += (tend.tv_usec - tstart.tv_usec);
  description->in_restore.tv_sec += (tend.tv_sec - tstart.tv_sec);
#endif

  return GASPI_SUCCESS;
}

gaspi_return_t
gpi_cp_read_buddy( const gpi_cp_description_t description
                 , const gaspi_timeout_t timeout_ms )
{
  /* Get from receiver */
  GASPI_SUCCESS_OR_RETURN ( gaspi_read
                      ( description->segment_id_local_for_sender
                        , description->active_snapshot
                        , description->receiver
                        , description->segment_id_remote_on_receiver
                        , (gaspi_offset_t) description->size - description->active_snapshot
                        , description->size
                        , description->queue
                        , timeout_ms
                        )
                 );
  GASPI_SUCCESS_OR_RETURN(gaspi_wait(description->queue, timeout_ms));

  return GASPI_SUCCESS;
}

bool
gpi_cp_get_state_in_progress(const gpi_cp_description_t description)
{
  return description->state_in_progress;
}

gaspi_offset_t
gpi_cp_get_active_snapshot(const gpi_cp_description_t description)
{
  return description->active_snapshot;
}

gaspi_pointer_t
gpi_cp_get_receiver_ptr(const gpi_cp_description_t description)
{
  gaspi_pointer_t receiver_seg;
  gaspi_segment_ptr(description->segment_id_local_for_sender, &receiver_seg);
  return receiver_seg;
}
