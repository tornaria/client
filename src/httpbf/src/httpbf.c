/*
 * httpbf - send big files via http
 *
 * Copyright (c) 2012 Klaas Freitag <freitag@owncloud.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation version 2 of
 * of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/timeb.h>
#include <sys/time.h>

#include <neon/ne_session.h>
#include <neon/ne_request.h>


#include "httpbf.h"

#ifdef NDEBUG
#define DEBUG_HBF(...)
#else
#define DEBUG_HBF(...) printf(__VA_ARGS__)
#endif

#define DEFAULT_BLOG_SIZE (1024*1024)

static int get_transfer_block_size() {
  return DEFAULT_BLOG_SIZE;
}

static int transfer_id( struct stat *sb ) {
    struct timeval tp;
    int res;
    int r;

    if( gettimeofday(&tp, 0) < 0 ) {
        return 0;
    }

    /* build a Unique ID:
     * take the current epoch and shift 8 bits up to keep the least bits.
     * than add the milliseconds, again shift by 8
     * and finally add the least 8 bit of the inode of the file.
     */
    res = tp.tv_sec; /* epoche value in seconds */
    res = res << 8;
    r = (sb->st_ino & 0xFF);
    res += r; /* least six bit of inode */
    res = res << sizeof(tp.tv_usec);
    res += tp.tv_usec; /* milliseconds */

    return res;
}

hbf_transfer_t *hbf_init_transfer( const char *dest_uri ) {
    hbf_transfer_t * transfer = NULL;

    transfer = malloc( sizeof(hbf_transfer_t) );
    memset(transfer, 0, sizeof(hbf_transfer_t));

    /* store the target uri */
    transfer->url = strdup(dest_uri);
    transfer->status_code = 200;
    transfer->error_string = NULL;
    transfer->start_id = 0;

    return transfer;
}

/* Create the splitlist of a given file descriptor */
Hbf_State hbf_splitlist(hbf_transfer_t *transfer, int fd ) {
  struct stat sb;
  off_t num_blocks;
  off_t blk_size;
  off_t remainder = 0;

  if( ! transfer ) {
      return HBF_PARAM_FAIL;
  }

  if( fd <= 0 ) {
    DEBUG_HBF("File descriptor is invalid.");
    return HBF_PARAM_FAIL;
  }
  
  if( fstat(fd, &sb) < 0 ) {
    DEBUG_HBF("Failed to stat the file descriptor: errno = %d", errno);
    return HBF_FILESTAT_FAIL;
  }
  
  
  transfer->fd = fd;
#ifdef NDEBUG
  transfer->stat_size = sb.st_size;
  transfer->calc_size = 0;
#endif

  /* calc the number of blocks to split in */
  blk_size = get_transfer_block_size();
  num_blocks = sb.st_size / blk_size;

  /* there migth be a remainder. */
  remainder = sb.st_size - num_blocks * blk_size;

  /* if there is a remainder, add one block */
  if( remainder > 0 ) {
      num_blocks++;
  }

  if( num_blocks ) {
      int cnt;
      off_t overall = 0;
      /* create a datastructure for the transfer data */
      transfer->block_arr = calloc(num_blocks, sizeof(hbf_block_t*));
      transfer->block_cnt = num_blocks;
      transfer->transfer_id = transfer_id(&sb);
      transfer->start_id = 0;

      for( cnt=0; cnt < num_blocks; cnt++ ) {
          /* allocate a block struct and fill */
          hbf_block_t *block = malloc( sizeof(hbf_block_t) );
          memset(block, 0, sizeof(hbf_block_t));

          block->seq_number = cnt;
          if( cnt > 0 ) {
              block->start = cnt * blk_size;
          }
          block->size  = blk_size;

          /* consider the remainder if we're already at the end */
          if( cnt == num_blocks-1 && remainder > 0 ) {
              block->size = remainder;
          }
          overall += block->size;
          /* store the block data into the result array in the transfer */
          *((transfer->block_arr)+cnt) = block;
      }
#ifdef NDEBUG
  transfer->calc_size = overall;
#endif
  }
  return HBF_SUCCESS;
}

void hbf_free_transfer( hbf_transfer_t *transfer ) {
    int cnt;

    if( !transfer ) return;

    for( cnt = 0; cnt < transfer->block_cnt; cnt++ ) {
        hbf_block_t *block = transfer->block_arr[cnt];
        if( block->http_error_msg ) free( block->http_error_msg );
        if( block ) free(block);
    }
    free( transfer->block_arr );
    free( transfer->url );

    if( transfer->error_string) free( (void*) transfer->error_string );

    free( transfer );
}
/* keep this function hidden if non debug. Public for unit test. */
#ifndef NDEBUG
static
#endif
char* get_transfer_url( hbf_transfer_t *transfer, int indx ) {
    char *res = NULL;

    hbf_block_t *block = NULL;

    if( ! transfer ) return NULL;
    block = transfer->block_arr[indx];
    if( ! block ) return NULL;

    if( asprintf(&res, "%s-chunking-%u-%u-%u", transfer->url, transfer->transfer_id,
            transfer->block_cnt, indx ) < 0 ) {
        return NULL;
    }
    return res;
}

static int dav_request( ne_request *req, int fd, hbf_block_t *blk ) {
    Hbf_State state = HBF_TRANSFER_SUCCESS;
    int res;
    const ne_status *req_status = NULL;
    const char *etag = NULL;

    if( ! (blk && req) ) return HBF_PARAM_FAIL;

    ne_set_request_body_fd(req, fd, blk->start, blk->size);
    /* printf("Start: %d and Size: %d\n", blk->start, blk->size ); */
    res = ne_request_dispatch(req);

    req_status = ne_get_status( req );

    switch(res) {
        case NE_OK: {
            state = HBF_TRANSFER_SUCCESS;
            etag = ne_get_response_header(req, "ETag");
            if (etag && etag[0])
                state = HBF_SUCCESS;
            break;
        }
        case NE_AUTH:
            state = HBF_AUTH_FAIL;
            break;
        case NE_PROXYAUTH:
            state = HBF_PROXY_AUTH_FAIL;
            break;
        case NE_CONNECT:
            state = HBF_CONNECT_FAIL;
            break;
        case NE_TIMEOUT:
            state = HBF_TIMEOUT_FAIL;
            break;
        case NE_ERROR:
            state = HBF_FAIL;
            break;
    }
    blk->state = state;
    blk->http_result_code = req_status->code;
    if( req_status->reason_phrase ) {
        blk->http_error_msg = strdup(req_status->reason_phrase);
    }

    return state;
}

Hbf_State hbf_transfer( ne_session *session, hbf_transfer_t *transfer, const char *verb ) {
    Hbf_State state = HBF_TRANSFER_SUCCESS;
    int cnt;
    int goOn = 1;

    if( ! session ) {
        state = HBF_SESSION_FAIL;
    }
    if( ! transfer ) {
        state = HBF_SPLITLIST_FAIL;
    }
    if( ! verb ) {
        state = HBF_PARAM_FAIL;
    }

    for( cnt=0; goOn && cnt < transfer->block_cnt; cnt++ ) {
        int block_id = (cnt + transfer->start_id) % transfer->block_cnt;
        hbf_block_t *block = transfer->block_arr[block_id];
        char *transfer_url = NULL;

        if( ! block ) state = HBF_PARAM_FAIL;

        if( state == HBF_TRANSFER_SUCCESS ) {
            transfer_url = get_transfer_url( transfer, block_id );
            if( ! transfer_url ) {
                state = HBF_PARAM_FAIL;
            }
        }
        if( state == HBF_TRANSFER_SUCCESS ) {
            ne_request *req = ne_request_create(session, "PUT", transfer_url);

            if( req ) {
                ne_add_request_header(req, "OC_CHUNKED", "1");
                state = dav_request( req, transfer->fd, transfer->block_arr[block_id] );

                if( state != HBF_TRANSFER_SUCCESS) {
                    transfer->start_id = block_id  % transfer->block_cnt;
                    transfer->error_string = strdup( ne_get_error(session) );
                    /* Set the code of the last transmission. */
                    transfer->status_code = transfer->block_arr[block_id]->http_result_code;
                    goOn = 0;
                }
                ne_request_destroy(req);
            } else {
                state = HBF_MEMORY_FAIL;
                goOn = 0;
            }
            free( transfer_url );
        }
    }
    return state;
}


const char *hbf_error_string( Hbf_State state )
{
    const char *re;
    switch( state ) {
    case HBF_SUCCESS:
        re = "Ok.";
        break;
    case HBF_NOT_TRANSFERED:   /* never tried to transfer     */
        re = "Block was not yet tried to transfer.";
        break;
    case HBF_TRANSFER:         /* transfer currently running  */
        re = "Block is currently transfered.";
        break;
    case HBF_TRANSFER_FAILED:  /* transfer tried but failed   */
        re = "Block transfer failed.";
        break;
    case HBF_TRANSFER_SUCCESS: /* transfer succeeded.         */
        re = "Block transfer successful.";
        break;
    case HBF_SPLITLIST_FAIL:   /* the file could not be split */
        re = "Splitlist could not be computed.";
        break;
    case HBF_SESSION_FAIL:
        re = "No valid session in transfer.";
        break;
    case HBF_FILESTAT_FAIL:
        re = "Source file could not be stat'ed.";
        break;
    case HBF_PARAM_FAIL:
        re = "Parameter fail.";
        break;
    case HBF_AUTH_FAIL:
        re = "Authentication fail.";
        break;
    case HBF_PROXY_AUTH_FAIL:
        re = "Proxy Authentication fail.";
        break;
    case HBF_CONNECT_FAIL:
        re = "Connection could not be established.";
        break;
    case HBF_TIMEOUT_FAIL:
        re = "Network timeout.";
        break;
    case HBF_MEMORY_FAIL:
        re = "Out of memory.";
        break;
    case HBF_FAIL:
    default:
        re = "Unknown error.";
    }
    return re;
}
