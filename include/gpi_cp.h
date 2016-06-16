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


/**
 * @file   gpi_cp.h
 *
 *
 * 
 * @brief  The GPI checkpointing library interface.
 * 
 * 
 */

#ifndef _GPI_CP_H_
#define _GPI_CP_H_

#include <GASPI.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Types */
    typedef struct gpi_cp_description* gpi_cp_description_t;

/**
 * Communication policies.
 * 
 */
    typedef enum
    {
        GPI_CP_POLICY_RING = 1 /* simple ring communication  */
    }  gpi_cp_policy_t;

/**
 * Functions return type.
 * 
 */
    typedef enum
    {
        GPI_CP_SUCCESS = 19000,
        GPI_CP_ERROR_UNEXPECTED_SEGMENT_ID_SOURCE = 19001,
        GPI_CP_ERROR_UNEXPECTED_SEGMENT_ID_RECEIVER = 19002,
        GPI_CP_ERROR_UNDEFINED_RANK = 19003,
    } gpi_cp_error_codes;

/** Initialise checkpoint description
 *
 * memory for this structure is allocated and initialized with default values
 *
 * \note after usage dealocate memory by free
 * \out gpi_cp_description_t
 */
    gpi_cp_description_t GPI_CP_DESCRIPTION_INITIALIZER();

/** Get version number.
 *
 *
 * @param version Output parameter with version number.
 *
 * @return GASPI_SUCCESS in case of success, GASPI_ERROR in case of error.
 */
    gaspi_return_t gpi_cp_version (float *version);

/** Initialise checkpoint
 *
 * will create a segment of size '2 * size' (locally to allow buddies to store data)
 *
 * \todo integrate with gaspi_error_str
 * \note global operation
 * \param segment_id_checkpoint:
 *            is a local value, possibly different on different ranks
 *            size (segment_id_checkpoint) >= size, or else undefined
 * \param offset:
 *            local value, possibly different on different ranks
 * \param size:
 *            required to be the same on all ranks
 *            undefined for size == 0
 * \param queue:
 *            local value, checkpoint_start and checkpoint_commit are working with
 *            that queue only
 *            queue can be used by application but idealy reserve it for
 *            checkpointing only
 * \param policy:
 *            Communication policy and pattern is required to be the same on all ranks
 * \param group:
 *            contains all working members
 * \param gaspi_timeout_t:
 *             timeout in milliseconds (or GASPI_BLOCK/GASPI_TEST)
 * \out gpi_cp_description_t
 */
    gaspi_return_t
    gpi_cp_init ( const gaspi_segment_id_t segment_id_checkpoint
                , const gaspi_offset_t offset
                , const gaspi_size_t size
                , const gaspi_queue_id_t queue
                , const gpi_cp_policy_t policy
                , const gaspi_group_t group
                , gpi_cp_description_t
                , const gaspi_timeout_t timeout_ms
                );

/** Initiate checkpointing and copy to remote segments
 *
 * copies all data from segment_id_checkpoint intervall [offset, offset + size)
 *
 * \note undefined are two checkpoint_start (d) without checkpoint_commit (d) in between
 * \param gpi_cp_description_t:
 *             description of the checkpoint memory layout
 * \param gaspi_timeout_t:
 *             timeout in milliseconds (or GASPI_BLOCK/GASPI_TEST)
 * \out gpi_cp_description_t:
 *             status of the current checkpointing process
 */
    gaspi_return_t
    gpi_cp_start ( gpi_cp_description_t
                 , const gaspi_timeout_t timeout_ms
                 );
 
/** Commit checkpointing
 *
 * wait for the current checkpoint to be created and make sure
 * that the corresponding data has been copied.
 *
 * \note global operation
 * \post the last checkpoint_start has been finished on all ranks
 *       => there is a complete snapshot available
 * \param gpi_cp_description_t:
 *             description of the checkpoint memory layout
 * \param gaspi_timeout_t:
 *             timeout in milliseconds (or GASPI_BLOCK/GASPI_TEST)
 * \out gpi_cp_description_t:
 *             status of the current checkpointing process
 */
    gaspi_return_t
    gpi_cp_commit ( gpi_cp_description_t description
                  , const gaspi_timeout_t timeout_ms
                  );
 
 /** Restore checkpoint
 *
 *
 * \note global operation, call from every member in the new_group
 * \todo probably fails in case two consecutive nodes (wrt topology) failed
 * \param segment_id_checkpoint:
 *            is a local value, possibly different on different ranks
 *            size (segment_id_checkpoint) >= size, or else undefined
 * \param offset:
 *            local value, possibly different on different ranks
 * \param size:
 *            required to be the same on all ranks
 *            undefined for size == 0
 * \param queue:
 *            local value, checkpoint_start and checkpoint_commit are working with
 *            that queue only
 *            queue can be used by application but idealy reserve it for
 *            checkpointing only
 * \param policy:
 *            Communication policy and pattern is required to be the same on all ranks
 * \param new_group:
 *            contains the new working members, different from the group 
 *            used in gpi_cp_init but of the same size
 * \param description is IN and OUT
 *            - on survivors: put in the old description and get updates
 *            - on joiners: put in an empty description like in init
 * \param gaspi_timeout_t:
 *             timeout in milliseconds (or GASPI_BLOCK/GASPI_TEST)
 * \post - description up to data (== checkpointing works again)
 *       - data from the last consistent snapshots (== successful commit)
 *         has been restored into the provided memory region
 */
    gaspi_return_t
    gpi_cp_restore ( const gaspi_segment_id_t segment_id_checkpoint
                   , const gaspi_offset_t offset
                   , const gaspi_size_t size
                   , const gaspi_queue_id_t queue
                   , const gpi_cp_policy_t policy
                   , const gaspi_group_t new_group
                   , gpi_cp_description_t description
                   , const gaspi_timeout_t timeout_ms
                   );

/** frees checkpoint segment
 *
 * \note undefined behavior when checkpoint_start still in progress
 *
 * \param gpi_cp_description_t:
 *             description of the checkpoint memory layout
 * \param gaspi_timeout_t:
 *             timeout in milliseconds (or GASPI_BLOCK/GASPI_TEST)
 */
    gaspi_return_t
    gpi_cp_finalize ( const gpi_cp_description_t description
                    , const gaspi_timeout_t timeout_ms
                    );



/**
 * Expert functions.
 * 
 */

/** read checkpointed data from buddy
 *
 * \param gpi_cp_description_t:
 *             description of the checkpoint memory layout
 * \param gaspi_timeout_t:
 *             timeout in milliseconds (or GASPI_BLOCK/GASPI_TEST)
 * \return GASPI_SUCCESS in case of success, GASPI_ERROR in case of error.
 */
    gaspi_return_t
    gpi_cp_read_buddy( const gpi_cp_description_t description
                     , const gaspi_timeout_t timeout_ms );

/** get state of checkpointing
 *
 * \param gpi_cp_description_t:
 *             description of the checkpoint memory layout
 * \return true if checkpointing in progress, false otherwise
 */
    bool
    gpi_cp_get_state_in_progress( const gpi_cp_description_t description );

/** get memory offset of the current active snapshot data
 *
 * \param gpi_cp_description_t:
 *             description of the checkpoint memory layout
 * \return gaspi_offset_t
 */
    gaspi_offset_t
    gpi_cp_get_active_snapshot( const gpi_cp_description_t description );

/** get pointer the memory segment containing active snapshot data
 *
 * \param gpi_cp_description_t:
 *             description of the checkpoint memory layout
 * \return gaspi_pointer_t
 */
    gaspi_pointer_t
    gpi_cp_get_receiver_ptr( const gpi_cp_description_t description );


/**
 * Utility functions.
 * 
 */

/** determine id of an unused segment
 *
 * \param gaspi_segment_id_t:
 *             Output parameter with the segment id.
 * \return GASPI_SUCCESS in case of success, GASPI_ERROR in case of error.
 */
    gaspi_return_t
    gpi_cp_get_unused_segment_id (gaspi_segment_id_t* unused_segment_id );

#ifdef __cplusplus
}
#endif

#endif //_GPI_CP_H_
