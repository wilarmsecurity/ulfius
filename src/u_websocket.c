/**
 * 
 * Ulfius Framework
 * 
 * REST framework library
 * 
 * u_websocket.c: websocket implementation
 * 
 * Copyright 2017-2018 Nicolas Mora <mail@babelouest.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation;
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU GENERAL PUBLIC LICENSE for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library.  If not, see <http://www.gnu.org/licenses/>.
 * 
 */

#include "u_private.h"
#include "ulfius.h"

#ifndef U_DISABLE_WEBSOCKET
#include "yuarel.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdlib.h>
#include <gnutls/crypto.h>

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

/**********************************/
/** Internal websocket functions **/
/**********************************/

static int is_websocket_data_available(struct _websocket_manager * websocket_manager) {
  int ret = 0, poll_ret = 0;
  
  poll_ret = poll(&websocket_manager->fds, 1, U_WEBSOCKET_USEC_WAIT);
  if (poll_ret == -1) {
    y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error poll websocket read");
    websocket_manager->connected = 0;
  } else if (websocket_manager->fds.revents & (POLLRDHUP|POLLERR|POLLHUP|POLLNVAL)) {
    websocket_manager->connected = 0;
  } else if (poll_ret > 0) {
    ret = 1;
  }
  return ret;
}

static ssize_t read_data_from_socket(struct _websocket_manager * websocket_manager, uint8_t * data, size_t len) {
  ssize_t ret = 0, data_len;
  
  if (len > 0) {
    do {
      if (websocket_manager->tls) {
        data_len = gnutls_record_recv(websocket_manager->gnutls_session, data, (len - ret));
      } else if (websocket_manager->type == U_WEBSOCKET_SERVER) {
        data_len = read(websocket_manager->mhd_sock, data, (len - ret));
      } else {
        data_len = read(websocket_manager->tcp_sock, data, (len - ret));
      }
      if (data_len > 0) {
        ret += data_len;
      } else if (data_len < 0) {
        ret = -1;
        break;
      }
    } while (ret < (ssize_t)len);
  }
  return ret;
}

/**
 * Workaround to make sure a message, as long as it can be is complete sent
 */
static void ulfius_websocket_send_frame(struct _websocket_manager * websocket_manager, const uint8_t * data, size_t len) {
  ssize_t ret = 0, off;
  if (data != NULL && len > 0) {
    for (off = 0; (size_t)off < len; off += ret) {
      if (websocket_manager->type == U_WEBSOCKET_SERVER) {
        ret = send(websocket_manager->mhd_sock, &data[off], len - off, MSG_NOSIGNAL);
        if (ret < 0) {
          break;
        }
      } else {
        if (websocket_manager->tls) {
          ret = gnutls_record_send(websocket_manager->gnutls_session, &data[off], len - off);
        } else {
          ret = send(websocket_manager->tcp_sock, &data[off], len - off, MSG_NOSIGNAL);
        }
        if (ret < 0) {
          break;
        }
      }
    }
  }
}

/**
 * Builds a websocket frame from the given struct _websocket_message
 * returns U_OK on success
 * frame must be free'd after use
 */
static int ulfius_build_frame (const struct _websocket_message * message,
                               const uint64_t data_offset,
                               const uint64_t data_len,
                               uint8_t ** frame,
                               size_t * frame_len) {
  int ret, has_fin = 0;
  unsigned int i;
  uint64_t off, frame_data_len;
  if (message != NULL && frame != NULL && frame_len != NULL) {
    *frame_len = 2;
    if (message->data_len > 65536) {
      *frame_len += 8;
    } else if (message->data_len > 125) {
      *frame_len += 2;
    }
    if (message->has_mask) {
      *frame_len += 4;
    }
    if (data_offset + data_len >= message->data_len) {
      frame_data_len = message->data_len - data_offset;
      has_fin = 1;
    } else {
      frame_data_len = data_len;
    }
    *frame_len += frame_data_len;
    *frame = o_malloc(*frame_len);
    if (*frame != NULL) {
      if (has_fin) {
        (*frame)[0] = (message->opcode | U_WEBSOCKET_BIT_FIN);
      } else {
        (*frame)[0] = 0;
      }
      if (message->data_len > 65536) {
        (*frame)[1] = 127;
        (*frame)[2] = (uint8_t)(frame_data_len >> 54);
        (*frame)[3] = (uint8_t)(frame_data_len >> 48);
        (*frame)[4] = (uint8_t)(frame_data_len >> 40);
        (*frame)[5] = (uint8_t)(frame_data_len >> 32);
        (*frame)[6] = (uint8_t)(frame_data_len >> 24);
        (*frame)[7] = (uint8_t)(frame_data_len >> 16);
        (*frame)[8] = (uint8_t)(frame_data_len >> 8);
        (*frame)[9] = (uint8_t)(frame_data_len);
        off = 10;
      } else if (data_len > 125) {
        (*frame)[1] = 126;
        (*frame)[2] = (uint8_t)(frame_data_len >> 8);
        (*frame)[3] = (uint8_t)(frame_data_len);
        off = 4;
      } else {
        (*frame)[1] = (uint8_t)frame_data_len;
        off = 2;
      }
      if (message->has_mask) {
        (*frame)[1] |= U_WEBSOCKET_MASK;
        // Append mask
        memcpy(*frame + off, message->mask, 4);
        off += 4;
        for (i=0; i < frame_data_len; i++) {
          (*frame)[off + i] = message->data[data_offset + i] ^ message->mask[i%4];
        }
      } else {
        memcpy((*frame) + off, message->data + data_offset, frame_data_len);
      }
      ret = U_OK;
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error allocating resources for *frame");
      ret = U_ERROR_MEMORY;
    }
  } else {
    ret = U_ERROR_PARAMS;
  }
  return ret;
}

/**
 * Builds a struct _websocket_message using the given parameters
 * returns a newly allocated struct _websocket_message
 * returned value must be free'd after use
 */
static struct _websocket_message * ulfius_build_message (const uint8_t opcode,
                                                         const short int has_mask,
                                                         const char * data,
                                                         const uint64_t data_len) {
  struct _websocket_message * new_message = NULL;
  if ((
       opcode == U_WEBSOCKET_OPCODE_TEXT ||
       opcode == U_WEBSOCKET_OPCODE_BINARY ||
       opcode == U_WEBSOCKET_OPCODE_CLOSE ||
       opcode == U_WEBSOCKET_OPCODE_PING ||
       opcode == U_WEBSOCKET_OPCODE_PONG
     ) &&
     (data_len == 0 || data != NULL)) {
    new_message = o_malloc(sizeof(struct _websocket_message));
    if (new_message != NULL) {
      if (data_len) {
        new_message->data = o_malloc(data_len*sizeof(char));
      } else {
        new_message->data = NULL;
      }
      if (!data_len || new_message->data != NULL) {
        new_message->opcode = opcode;
        new_message->data_len = data_len;
        if (!has_mask) {
          new_message->has_mask = 0;
          memset(new_message->mask, 0, 4);
        } else {
          gnutls_rnd(GNUTLS_RND_NONCE, &new_message->mask, 4*sizeof(uint8_t));
          new_message->has_mask = 1;
        }
        if (data_len > 0) {
          memcpy(new_message->data, data, data_len);
        }
        time(&new_message->datestamp);
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error allocating resources for new_message->data");
        o_free(new_message);
        new_message = NULL;
      }
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error allocating resources for new_message");
    }
  }
  return new_message;
}

/**
 * Builds a struct _websocket_message using the given parameters
 * Sends message to the websocket recipient in fragment if required
 * Then pushes the message in the outcoming message list
 * returns U_OK on success
 */
static int ulfius_send_websocket_message_managed(struct _websocket_manager * websocket_manager,
                                                 const uint8_t opcode,
                                                 const uint64_t data_len,
                                                 const char * data,
                                                 const uint64_t fragment_len) {
  size_t offset = 0, cur_len;
  struct _websocket_message * message;
  uint8_t * frame = NULL;
  size_t frame_len = 0;
  int ret = U_OK;
  
  if (data != NULL || data_len == 0) {
    if (pthread_mutex_lock(&websocket_manager->write_lock)) {
      y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error locking write lock");
    } else {
      message = ulfius_build_message(opcode, (websocket_manager->type == U_WEBSOCKET_CLIENT), data, data_len);
      if (message != NULL) {
        //if (ulfius_push_websocket_message(websocket_manager->message_list_outcoming, message) != U_OK) {
        //  y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error pushing new websocket message in list");
        //}
        while (offset < data_len) {
          cur_len = fragment_len<(data_len - offset)?fragment_len:(data_len - offset);
          if ((ret = ulfius_build_frame(message, offset, cur_len, &frame, &frame_len)) != U_OK) {
            y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error ulfius_build_frame");
            break;
          } else {
            ulfius_websocket_send_frame(websocket_manager, frame, frame_len);
            offset += cur_len;
            o_free(frame);
            frame = NULL;
            frame_len = 0;
          }
        }
        ulfius_clear_websocket_message(message);
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error ulfius_build_message");
        ret = U_ERROR;
      }
      pthread_mutex_unlock(&websocket_manager->write_lock);
    }
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error ulfius_send_websocket_message_managed params");
    ret = U_ERROR_PARAMS;
  }
  return ret;
}

/**
 * Read and parse a new message from the websocket
 * Return the opcode of the new websocket, U_WEBSOCKET_OPCODE_NONE if no message arrived, or U_WEBSOCKET_OPCODE_ERROR on error
 * Sets the new message in the message variable
 */
static int ulfius_read_incoming_message(struct _websocket_manager * websocket_manager, struct _websocket_message ** message) {
  int ret = U_OK, fin = 0, i;
  uint8_t header[2] = {0}, payload_len[8] = {0}, masking_key[4] = {0};
  uint8_t * payload_data = NULL;
  size_t msg_len = 0;
  ssize_t len = 0;
  
  *message = o_malloc(sizeof(struct _websocket_message));
  if (*message != NULL) {
    (*message)->data_len = 0;
    (*message)->has_mask = 0;
    (*message)->data = NULL;
    time(&(*message)->datestamp);
    
    do {
      if (ret == U_OK) {
        // Read header
        if ((len = read_data_from_socket(websocket_manager, header, 2)) == 2) {
          (*message)->opcode = header[0] & 0x0F;
          fin = (header[0] & U_WEBSOCKET_BIT_FIN);
          if ((header[1] & U_WEBSOCKET_LEN_MASK) <= 125) {
            msg_len = (header[1] & U_WEBSOCKET_LEN_MASK);
          } else if ((header[1] & U_WEBSOCKET_LEN_MASK) == 126) {
            len = read_data_from_socket(websocket_manager, payload_len, 2);
            if (len == 2) {
              msg_len = payload_len[1] | ((uint64_t)payload_len[0] << 8);
            } else if (len >= 0) {
              ret = U_ERROR;
              y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error reading websocket message length");
            } else {
              ret = U_ERROR_DISCONNECTED;
            }
          } else if ((header[1] & U_WEBSOCKET_LEN_MASK) == 127) {
            len = read_data_from_socket(websocket_manager, payload_len, 8);
            if (len == 8) {
              msg_len = payload_len[7] |
                        ((uint64_t)payload_len[6] << 8) |
                        ((uint64_t)payload_len[5] << 16) |
                        ((uint64_t)payload_len[4] << 24) |
                        ((uint64_t)payload_len[3] << 32) |
                        ((uint64_t)payload_len[2] << 40) |
                        ((uint64_t)payload_len[1] << 48) |
                        ((uint64_t)payload_len[0] << 54);
            } else if (len >= 0) {
              ret = U_ERROR;
              y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error reading websocket message length");
            } else {
              ret = U_ERROR_DISCONNECTED;
            }
          }
        } else if (len == 0) {
          y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error reading websocket");
          ret = U_ERROR;
        } else {
          ret = U_ERROR_DISCONNECTED;
        }
        
        if (ret == U_OK) {
          if (websocket_manager->type == U_WEBSOCKET_SERVER) {
            // Read mask
            if (header[1] & U_WEBSOCKET_MASK) {
              (*message)->has_mask = 1;
              len = read_data_from_socket(websocket_manager, masking_key, 4);
              if (len != 4 && len >= 0) {
                y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error reading websocket for mask");
                ret = U_ERROR;
              } else if (len < 0) {
                ret = U_ERROR_DISCONNECTED;
              }
            } else {
              y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Incoming message has no MASK flag, exiting");
              ret = U_ERROR;
            }
          } else {
            if ((header[1] & U_WEBSOCKET_MASK)) {
              y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Incoming message has MASK flag while it should not, exiting");
              ret = U_ERROR;
            }
          }
        }
        if (ret == U_OK) {
          payload_data = o_malloc(msg_len*sizeof(uint8_t));
          len = read_data_from_socket(websocket_manager, payload_data, msg_len);
          if (len < 0) {
            ret = U_ERROR_DISCONNECTED;
          } else if ((unsigned int)len != msg_len) {
            y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error reading websocket for payload_data");
            ret = U_ERROR;
          } else {
            // If mask, decode message
            (*message)->data = o_realloc((*message)->data, (msg_len+(*message)->data_len)*sizeof(uint8_t));
            if ((*message)->has_mask) {
              for (i = (*message)->data_len; (unsigned int)i < (*message)->data_len + msg_len; i++) {
                (*message)->data[i] = payload_data[i-(*message)->data_len] ^ masking_key[(i-(*message)->data_len)%4];
              }
            } else {
              memcpy((*message)->data+(*message)->data_len, payload_data, msg_len);
            }
            (*message)->data_len += msg_len;
          }
          o_free(payload_data);
        }
        if (!fin) {
          while (!is_websocket_data_available(websocket_manager));
        }
      }
    } while (ret == U_OK && !fin);
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error allocating resources for *message");
  }
  if (ret != U_OK) {
    ulfius_clear_websocket_message(*message);
  }
  return ret;
}

/**
 * Run the websocket manager in a separated detached thread
 */
void * ulfius_thread_websocket_manager_run(void * args) {
  struct _websocket * websocket = (struct _websocket *)args;
  if (websocket != NULL) {
    websocket->websocket_manager_callback(websocket->request, websocket->websocket_manager, websocket->websocket_manager_user_data);
    
    // Send close message if the websocket is still open
    if (websocket->websocket_manager->connected) {
      websocket->websocket_manager->close_flag = 1;
    }
  }
  return NULL;
}

/**
 * Run websocket in a separate thread
 * then sets a listening message loop
 * Complete the callback when the websocket is closed
 * The websocket can be closed by the client, the manager, the program, or on network disconnect
 */
void * ulfius_thread_websocket(void * data) {
  struct _websocket * websocket = (struct _websocket*)data;
  struct _websocket_message * message = NULL;
  pthread_t thread_websocket_manager;
  int thread_ret_websocket_manager = 1;
  
  if (websocket != NULL && websocket->websocket_manager != NULL) {
    if (websocket->websocket_manager_callback != NULL) {
      thread_ret_websocket_manager = pthread_create(&thread_websocket_manager, NULL, ulfius_thread_websocket_manager_run, (void *)websocket);
      if (thread_ret_websocket_manager) {
        y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error creating websocket manager thread, return code: %d", thread_ret_websocket_manager);
        websocket->websocket_manager->connected = 0;
      }
    }
    while (websocket->websocket_manager->connected) {
      message = NULL;
      if (websocket->websocket_manager->close_flag) {
        if (ulfius_websocket_send_message(websocket->websocket_manager, U_WEBSOCKET_OPCODE_CLOSE, 0, NULL) != U_OK) {
          y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error sending close message on close_flag");
        }
        websocket->websocket_manager->connected = 0;
      } else {
        if (is_websocket_data_available(websocket->websocket_manager)) {
          if (pthread_mutex_lock(&websocket->websocket_manager->read_lock)) {
            y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error locking websocket read lock messages");
            websocket->websocket_manager->connected = 0;
          } else {
            if (ulfius_read_incoming_message(websocket->websocket_manager, &message) == U_OK) {
              if (message->opcode == U_WEBSOCKET_OPCODE_CLOSE) {
                // Send close command back, then close the socket
                if (ulfius_send_websocket_message_managed(websocket->websocket_manager, U_WEBSOCKET_OPCODE_CLOSE, 0, NULL, 0) != U_OK) {
                  y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error sending close command");
                }
                websocket->websocket_manager->connected = 0;
              } else if (message->opcode == U_WEBSOCKET_OPCODE_PING) {
                // Send pong command
                if (ulfius_websocket_send_message(websocket->websocket_manager, U_WEBSOCKET_OPCODE_PONG, 0, NULL) != U_OK) {
                  y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error sending pong command");
                  websocket->websocket_manager->connected = 0;
                }
              }
              if (websocket->websocket_incoming_message_callback != NULL) {
                websocket->websocket_incoming_message_callback(websocket->request, websocket->websocket_manager, message, websocket->websocket_incoming_user_data);
              }
              //if (ulfius_push_websocket_message(websocket->websocket_manager->message_list_incoming, message) != U_OK) {
              //  y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error pushing new websocket message in list");
              //  websocket->websocket_manager->connected = 0;
              //}
            } else {
              y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error ulfius_read_incoming_message");
              websocket->websocket_manager->connected = 0;
            }
            pthread_mutex_unlock(&websocket->websocket_manager->read_lock);
          }
        }
      }
    }
    // Wait for thread manager to close if exists
    if (!thread_ret_websocket_manager) {
      pthread_join(thread_websocket_manager, NULL);
    }
    // Call websocket_onclose_callback if set
    if (websocket->websocket_onclose_callback != NULL) {
      websocket->websocket_onclose_callback(websocket->request, websocket->websocket_manager, websocket->websocket_onclose_user_data);
    }
    if (ulfius_close_websocket(websocket) != U_OK) {
      y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error closing websocket");
    }
    // Broadcast end signal
    if (websocket->websocket_manager->type == U_WEBSOCKET_CLIENT) {
      pthread_mutex_lock(&websocket->websocket_manager->status_lock);
      pthread_cond_broadcast(&websocket->websocket_manager->status_cond);
      pthread_mutex_unlock(&websocket->websocket_manager->status_lock);
    } else if (websocket->websocket_manager->type == U_WEBSOCKET_SERVER) {
      ulfius_clear_websocket(websocket);
    }
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error websocket parameters");
  }
  pthread_exit(NULL);
}

/**
 * Read the next line in http response
 * Fill buffer until \r\n is read or buffer_len is reached
 * return U_OK on success
 */
static int ulfius_get_next_line_from_http_response(struct _websocket * websocket, char * buffer, size_t buffer_len, size_t * line_len) {
  size_t offset = 0;
  int eol = 0, ret = U_ERROR;
  uint8_t car;
  
  *line_len = 0;
  do {
    if (read_data_from_socket(websocket->websocket_manager, &car, 1) == 1) {
      buffer[offset] = car;
    }
    
    if (offset > 0 && buffer[offset-1] == '\r' && buffer[offset] == '\n') {
      eol = 1;
      buffer[offset-1] = '\0';
      *line_len = offset - 1;
      ret = U_OK;
    }
    
    offset++;
  } while (!eol && offset < buffer_len);
  return ret;
}

/**
 * Sends the HTTP request and read the HTTP response
 * Verify if the response has the expexted paramters to open the
 * websocket
 * Return U_OK on success
 */
static int ulfius_websocket_connection_handshake(struct _u_request * request, struct yuarel * y_url, struct _websocket * websocket, struct _u_response * response) {
  int websocket_response_http = 0, i, ret, check_websocket = WEBSOCKET_RESPONSE_UPGRADE | WEBSOCKET_RESPONSE_CONNECTION | WEBSOCKET_RESPONSE_ACCEPT;
  unsigned int websocket_response = 0;
  char * http_line, ** split_line = NULL, * key, * value, * separator;
  char buffer[4096] = {0};
  const char ** keys;
  size_t buffer_len = 4096, line_len;
  
  // Send HTTP Request
  http_line = msprintf("%s /%s%s%s HTTP/%s\r\n", request->http_verb, o_strlen(y_url->path)?y_url->path:"", y_url->query!=NULL?"?":"", y_url->query!=NULL?y_url->query:"", request->http_protocol);
  ulfius_websocket_send_frame(websocket->websocket_manager, (uint8_t *)http_line, o_strlen(http_line));
  o_free(http_line);
  
  http_line = msprintf("Host: %s\r\n", y_url->host);
  ulfius_websocket_send_frame(websocket->websocket_manager, (uint8_t *)http_line, o_strlen(http_line));
  o_free(http_line);
  
  http_line = msprintf("Upgrade: websocket\r\n");
  ulfius_websocket_send_frame(websocket->websocket_manager, (uint8_t *)http_line, o_strlen(http_line));
  o_free(http_line);
  
  http_line = msprintf("Connection: Upgrade\r\n");
  ulfius_websocket_send_frame(websocket->websocket_manager, (uint8_t *)http_line, o_strlen(http_line));
  o_free(http_line);
  
  http_line = msprintf("Origin: %s://%s\r\n", y_url->scheme, y_url->host);
  ulfius_websocket_send_frame(websocket->websocket_manager, (uint8_t *)http_line, o_strlen(http_line));
  o_free(http_line);
  
  keys = u_map_enum_keys(request->map_header);
  for (i=0; keys[i] != NULL; i++) {
    http_line = msprintf("%s: %s\r\n", keys[i], u_map_get(request->map_header, keys[i]));
    ulfius_websocket_send_frame(websocket->websocket_manager, (uint8_t *)http_line, o_strlen(http_line));
    o_free(http_line);
    if (0 == o_strcmp("Sec-WebSocket-Protocol", keys[i])) {
      check_websocket |= WEBSOCKET_RESPONSE_PROTCOL;
    } else if (0 == o_strcmp("Sec-WebSocket-Extension", keys[i])) {
      check_websocket |= WEBSOCKET_RESPONSE_EXTENSION;
    }
  }
  
  if (websocket->websocket_manager->tcp_sock >= 0) {
    // Send empty line
    const char * empty = "\r\n";
    ulfius_websocket_send_frame(websocket->websocket_manager, (uint8_t *)empty, o_strlen(empty));
  }
  
  // Read and parse response
  if (ulfius_get_next_line_from_http_response(websocket, buffer, buffer_len, &line_len) == U_OK) {
    if (split_string(buffer, " ", &split_line) >= 2 && 0 == o_strcmp(split_line[0], "HTTP/1.1") && 0 == o_strcmp(split_line[1], "101")) {
      websocket_response_http = 1;
      response->status = strtol(split_line[1], NULL, 10);
      response->protocol = o_strdup("1.1");
    }
    free_string_array(split_line);
  }
  if (websocket_response_http) {
    do {
      if (ulfius_get_next_line_from_http_response(websocket, buffer, buffer_len, &line_len) == U_OK) {
        if (o_strlen(buffer) && (separator = o_strchr(buffer, ':')) != NULL) {
          key = o_strndup(buffer, (separator - buffer));
          value = o_strdup(separator + 1);
          if (u_map_put(response->map_header, key, value) != U_OK) {
            y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error adding header %s:%s to the response structure", key, value);
          }
          if (0 == o_strcmp(buffer, "Upgrade: websocket")) {
            websocket_response |= WEBSOCKET_RESPONSE_UPGRADE;
          } else if (0 == o_strcmp(buffer, "Connection: Upgrade")) {
            websocket_response |= WEBSOCKET_RESPONSE_CONNECTION;
          } else if (0 == o_strcmp(key, "Sec-WebSocket-Protocol")) {
            websocket->websocket_manager->protocol = o_strdup(value);
            websocket_response |= WEBSOCKET_RESPONSE_PROTCOL;
          } else if (0 == o_strcmp(key, "Sec-WebSocket-Extension")) {
            websocket->websocket_manager->extensions = o_strdup(value);
            websocket_response |= WEBSOCKET_RESPONSE_EXTENSION;
          } else if (0 == o_strcmp(buffer, "Sec-WebSocket-Accept") && ulfius_check_handshake_response(u_map_get(request->map_header, "Sec-WebSocket-Key"), value) == U_OK) {
            websocket_response |= WEBSOCKET_RESPONSE_ACCEPT;
          }
          o_free(key);
          o_free(value);
        } else if (0 == o_strcmp(buffer, "")) {
          // Websocket HTTP response header complete
          break;
        }
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error ulfius_get_next_line_from_http_response, abort parsing response");
        close(websocket->websocket_manager->tcp_sock);
        websocket->websocket_manager->tcp_sock = -1;
        break;
      }
    } while (1);
  }
  
  if (!websocket_response_http || !(websocket_response & check_websocket) || response->status != 101) {
    if (u_map_has_key(response->map_header, "Content-Length")) {
      response->binary_body_length = strtol(u_map_get(response->map_header, "Content-Length"), NULL, 10);
      response->binary_body = o_malloc(response->binary_body_length);
      if (response->binary_body != NULL) {
        if (read_data_from_socket(websocket->websocket_manager, response->binary_body, response->binary_body_length) != (ssize_t)response->binary_body_length) {
          y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error read_data_from_socket for response->binary_body");
        }
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error allocating resources for response->binary_body");
      }
    }
    close(websocket->websocket_manager->tcp_sock);
    websocket->websocket_manager->tcp_sock = -1;
    ret = U_ERROR;
  } else {
    ret = U_OK;
  }
  
  return ret;
}

/**
 * Opens a websocket connection to the specified server
 * Returns U_OK on success
 */
static int ulfius_open_websocket(struct _u_request * request, struct yuarel * y_url, struct _websocket * websocket, struct _u_response * response) {
  int ret;
  struct sockaddr_in server;
  struct hostent * he;
  
  websocket->websocket_manager->tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (websocket->websocket_manager->tcp_sock != -1) {
    if ((he = gethostbyname(y_url->host)) != NULL) {
      memcpy(&server.sin_addr, he->h_addr_list[0], he->h_length);
      server.sin_family = AF_INET;
      server.sin_port = htons(y_url->port);
      
      if (connect(websocket->websocket_manager->tcp_sock, (struct sockaddr *)&server , sizeof(server)) >= 0) {
        websocket->websocket_manager->fds.fd = websocket->websocket_manager->tcp_sock;
        websocket->websocket_manager->connected = 1;
        websocket->websocket_manager->close_flag = 0;
        websocket->urh = NULL;
        websocket->instance = NULL;
        
        ret = ulfius_websocket_connection_handshake(request, y_url, websocket, response);
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error connecting socket");
        close(websocket->websocket_manager->tcp_sock);
        websocket->websocket_manager->tcp_sock = -1;
        ret = U_ERROR;
      }
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error gethostbyname");
      ret = U_ERROR;
    }
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error opening socket");
    ret = U_ERROR;
  }
  return ret;
}

/**
 * Opens a websocket connection to the specified server using a tls socket
 * Returns U_OK on success
 */
static int ulfius_open_websocket_tls(struct _u_request * request, struct yuarel * y_url, struct _websocket * websocket, struct _u_response * response) {
  int ret;
  struct sockaddr_in server;
  struct hostent * he;
  gnutls_datum_t out;
  int type;
  unsigned status;
  
  if (gnutls_global_init() >= 0) {
    if (gnutls_certificate_allocate_credentials(&websocket->websocket_manager->xcred) >= 0 &&
        gnutls_certificate_set_x509_system_trust(websocket->websocket_manager->xcred) >= 0 &&
        gnutls_init(&websocket->websocket_manager->gnutls_session, GNUTLS_CLIENT) >= 0 &&
        gnutls_server_name_set(websocket->websocket_manager->gnutls_session, GNUTLS_NAME_DNS, y_url->host, o_strlen(y_url->host)) >= 0 &&
        gnutls_set_default_priority(websocket->websocket_manager->gnutls_session) >= 0 &&
        gnutls_credentials_set(websocket->websocket_manager->gnutls_session, GNUTLS_CRD_CERTIFICATE, websocket->websocket_manager->xcred) >= 0) {
          
      if (request->check_server_certificate) {
        gnutls_session_set_verify_cert(websocket->websocket_manager->gnutls_session, y_url->host, 0);
      }
      websocket->websocket_manager->tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
      if (websocket->websocket_manager->tcp_sock != -1) {
        if ((he = gethostbyname(y_url->host)) != NULL) {
          memset(&server, '\0', sizeof(server));
          memcpy(&server.sin_addr, he->h_addr_list[0], he->h_length);
          server.sin_family = AF_INET;
          server.sin_port = htons(y_url->port);
          if (connect(websocket->websocket_manager->tcp_sock, (struct sockaddr *)&server , sizeof(server)) >= 0) {
            websocket->websocket_manager->fds.fd = websocket->websocket_manager->tcp_sock;
            websocket->websocket_manager->connected = 1;
            websocket->websocket_manager->close_flag = 0;
            websocket->urh = NULL;
            websocket->instance = NULL;
            
            gnutls_transport_set_int(websocket->websocket_manager->gnutls_session, websocket->websocket_manager->tcp_sock);
            gnutls_handshake_set_timeout(websocket->websocket_manager->gnutls_session, GNUTLS_DEFAULT_HANDSHAKE_TIMEOUT);

            do {
              ret = gnutls_handshake(websocket->websocket_manager->gnutls_session);
            }
            while (ret < 0 && gnutls_error_is_fatal(ret) == 0);
            
            if (ret < 0) {
              if (ret == GNUTLS_E_CERTIFICATE_VERIFICATION_ERROR) {
                /* check certificate verification status */
                type = gnutls_certificate_type_get(websocket->websocket_manager->gnutls_session);
                status = gnutls_session_get_verify_cert_status(websocket->websocket_manager->gnutls_session);
                if (gnutls_certificate_verification_status_print(status, type, &out, 0) >= 0) {
                  y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Certificate verify output: %s\n", out.data);
                  gnutls_free(out.data);
                } else {
                  y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error gnutls_certificate_verification_status_print");
                }
              }
              y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Handshake failed: %s\n", gnutls_strerror(ret));
              ret = U_ERROR;
            } else {
              char * desc = gnutls_session_get_desc(websocket->websocket_manager->gnutls_session);
              gnutls_free(desc);
              
              ret = ulfius_websocket_connection_handshake(request, y_url, websocket, response);
            }
          } else {
            y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error connecting socket");
            close(websocket->websocket_manager->tcp_sock);
            websocket->websocket_manager->tcp_sock = -1;
            ret = U_ERROR;
          }

        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error gethostbyname");
          ret = U_ERROR;
        }
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error opening socket");
        ret = U_ERROR;
      }
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error initialize gnutls routines");
      ret = U_ERROR;
    }
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error gnutls_global_init");
    ret = U_ERROR;
  }
  return ret;
}

/**
 * Websocket callback function for MHD
 * Starts the websocket manager if set,
 */
void ulfius_start_websocket_cb (void * cls,
                                struct MHD_Connection * connection,
                                void * con_cls,
                                const char * extra_in,
                                size_t extra_in_size,
                                MHD_socket sock,
                                struct MHD_UpgradeResponseHandle * urh) {
  struct _websocket * websocket = (struct _websocket *)cls;
  pthread_t thread_websocket;
  int thread_ret_websocket = 0, thread_detach_websocket = 0;
  UNUSED(connection);
  UNUSED(con_cls);
  UNUSED(extra_in);
  UNUSED(extra_in_size);
  
  if (websocket != NULL) {
    websocket->urh = urh;
    // Run websocket manager in a thread
    websocket->websocket_manager->type = U_WEBSOCKET_SERVER;
    websocket->websocket_manager->mhd_sock = sock;
    websocket->websocket_manager->fds.fd = sock;
    websocket->websocket_manager->fds.events = POLLIN | POLLRDHUP;
    websocket->websocket_manager->connected = 1;
    websocket->websocket_manager->close_flag = 0;
    thread_ret_websocket = pthread_create(&thread_websocket, NULL, ulfius_thread_websocket, (void *)websocket);
    thread_detach_websocket = pthread_detach(thread_websocket);
    if (thread_ret_websocket || thread_detach_websocket) {
      y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error creating or detaching websocket manager thread, return code: %d, detach code: %d",
                    thread_ret_websocket, thread_detach_websocket);
      if (websocket->websocket_onclose_callback != NULL) {
        websocket->websocket_onclose_callback(websocket->request, websocket->websocket_manager, websocket->websocket_onclose_user_data);
      }
      ulfius_clear_websocket(websocket);
    }
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error websocket is NULL");
    ulfius_clear_websocket(websocket);
  }
  return;
}

/**
 * Check if the response corresponds to the transformation of the key with the magic string
 */
int ulfius_check_handshake_response(const char * key, const char * response) {
  char websocket_accept[32] = {0};
  
  if (key != NULL && response != NULL) {
    if (ulfius_generate_handshake_answer(key, websocket_accept) && 0 == o_strcmp(websocket_accept, response)) {
      return U_OK;
    } else {
      return U_ERROR;
    }
  } else {
    return U_ERROR_PARAMS;
  }
}

/**
 * Generates a handhshake answer from the key given in parameter
 */
int ulfius_generate_handshake_answer(const char * key, char * out_digest) {
  gnutls_datum_t key_data;
  unsigned char encoded_key[32] = {0};
  size_t encoded_key_size = 32, encoded_key_size_base64;
  int res, to_return = 0;
  
  key_data.data = (unsigned char*)msprintf("%s%s", key, U_WEBSOCKET_MAGIC_STRING);
  key_data.size = o_strlen((const char *)key_data.data);
  
  if (key_data.data != NULL && out_digest != NULL && (res = gnutls_fingerprint(GNUTLS_DIG_SHA1, &key_data, encoded_key, &encoded_key_size)) == GNUTLS_E_SUCCESS) {
    if (o_base64_encode(encoded_key, encoded_key_size, (unsigned char *)out_digest, &encoded_key_size_base64)) {
      to_return = 1;
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error base64 encoding hashed key");
    }
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error getting sha1 signature for key");
  }
  o_free(key_data.data);
  return to_return;
}

/**
 * Initialize a websocket message list
 * Return U_OK on success
 */
int ulfius_init_websocket_message_list(struct _websocket_message_list * message_list) {
  if (message_list != NULL) {
    message_list->len = 0;
    message_list->list = NULL;
    return U_OK;
  } else {
    return U_ERROR_PARAMS;
  }
}

/**
 * Append a message in a message list
 * Return U_OK on success
 */
int ulfius_push_websocket_message(struct _websocket_message_list * message_list, struct _websocket_message * message) {
  if (message_list != NULL && message != NULL) {
    message_list->list = o_realloc(message_list->list, (message_list->len+1)*sizeof(struct _websocket_message *));
    if (message_list->list != NULL) {
      message_list->list[message_list->len] = message;
      message_list->len++;
      return U_OK;
    } else {
      return U_ERROR_MEMORY;
    }
  } else {
    return U_ERROR_PARAMS;
  }
}

/**
 * Return a match list between two list of items
 * If match is NULL, then return source duplicate
 * Returned value must be u_free'd after use
 */
int ulfius_check_list_match(const char * source, const char * match, const char * separator, char ** result) {
  char ** source_list = NULL, ** match_list = NULL;
  int i, ret = U_OK;
  
  if (result != NULL) {
    *result = NULL;
    if (match == NULL) {
      *result = o_strdup(source);
    } else {
      if (source != NULL) {
        if (split_string(source, separator, &source_list) > 0 && split_string(match, separator, &match_list) > 0) {
          for (i=0; source_list[i] != NULL; i++) {
            if (string_array_has_trimmed_value((const char **)match_list, source_list[i])) {
              if (*result == NULL) {
                *result = o_strdup(trimwhitespace(source_list[i]));
              } else {
                char * tmp = msprintf("%s%s%s", *result, separator, trimwhitespace(source_list[i]));
                o_free(*result);
                *result = tmp;
              }
            }
          }
          free_string_array(source_list);
          free_string_array(match_list);
        }
        if (*result == NULL) {
          ret = U_ERROR;
        }
      }
    }
  } else {
    ret = U_ERROR_PARAMS;
  }
  return ret;
}

/**
 * Return the first match between two list of items
 * If match is NULL, then return the first element of source
 * Returned value must be u_free'd after use
 */
int ulfius_check_first_match(const char * source, const char * match, const char * separator, char ** result) {
  char ** source_list = NULL, ** match_list = NULL;
  int i, ret = U_OK;
  
  if (result != NULL) {
    *result = NULL;
    if (match == NULL) {
      if (source != NULL) {
        if (split_string(source, separator, &source_list) > 0) {
          *result = o_strdup(trimwhitespace(source_list[0]));
        }
        free_string_array(source_list);
      }
    } else {
      if (source != NULL) {
        if (split_string(source, separator, &source_list) > 0 && split_string(match, separator, &match_list) > 0) {
          for (i=0; source_list[i] != NULL && *result == NULL; i++) {
            if (string_array_has_trimmed_value((const char **)match_list, source_list[i])) {
              if (*result == NULL) {
                *result = o_strdup(trimwhitespace(source_list[i]));
              }
            }
          }
          free_string_array(source_list);
          free_string_array(match_list);
        }
      }
      if (*result == NULL) {
        ret = U_ERROR;
      }
    }
  } else {
    ret = U_ERROR_PARAMS;
  }
  return ret;
}

/**
 * Close the websocket
 */
int ulfius_close_websocket(struct _websocket * websocket) {
  if (websocket != NULL && websocket->websocket_manager != NULL) {
    if (websocket->websocket_manager->type == U_WEBSOCKET_CLIENT && websocket->websocket_manager->tls) {
      gnutls_bye(websocket->websocket_manager->gnutls_session, GNUTLS_SHUT_RDWR);
      gnutls_deinit(websocket->websocket_manager->gnutls_session);
      gnutls_certificate_free_credentials(websocket->websocket_manager->xcred);
      gnutls_global_deinit();
    }
    if (websocket->websocket_manager->type == U_WEBSOCKET_CLIENT) {
      shutdown(websocket->websocket_manager->tcp_sock, SHUT_RDWR);
      close(websocket->websocket_manager->tcp_sock);
    }
    websocket->websocket_manager->connected = 0;
    return U_OK;
  } else {
    return U_ERROR_PARAMS;
  }
}

/**
 * Add a websocket in the list of active websockets of the instance
 */
int ulfius_instance_add_websocket_active(struct _u_instance * instance, struct _websocket * websocket) {
  if (instance != NULL && websocket != NULL) {
    ((struct _websocket_handler *)instance->websocket_handler)->websocket_active = o_realloc(((struct _websocket_handler *)instance->websocket_handler)->websocket_active, (((struct _websocket_handler *)instance->websocket_handler)->nb_websocket_active+1)*sizeof(struct _websocket *));
    if (((struct _websocket_handler *)instance->websocket_handler)->websocket_active != NULL) {
      ((struct _websocket_handler *)instance->websocket_handler)->websocket_active[((struct _websocket_handler *)instance->websocket_handler)->nb_websocket_active] = websocket;
      ((struct _websocket_handler *)instance->websocket_handler)->nb_websocket_active++;
      return U_OK;
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error allocating resources for instance->websocket_handler->websocket_active");
      return U_ERROR_MEMORY;
    }
  } else {
    return U_ERROR_PARAMS;
  }
}

/**
 * Remove a websocket from the list of active websockets of the instance
 */
int ulfius_instance_remove_websocket_active(struct _u_instance * instance, struct _websocket * websocket) {
  size_t i, j;
  if (instance != NULL && ((struct _websocket_handler *)instance->websocket_handler)->websocket_active != NULL && websocket != NULL) {
    for (i=0; i<((struct _websocket_handler *)instance->websocket_handler)->nb_websocket_active; i++) {
      if (((struct _websocket_handler *)instance->websocket_handler)->websocket_active[i] == websocket) {
        if (((struct _websocket_handler *)instance->websocket_handler)->nb_websocket_active > 1) {
          for (j=i; j<((struct _websocket_handler *)instance->websocket_handler)->nb_websocket_active-1; j++) {
            ((struct _websocket_handler *)instance->websocket_handler)->websocket_active[j] = ((struct _websocket_handler *)instance->websocket_handler)->websocket_active[j+1];
          }
          ((struct _websocket_handler *)instance->websocket_handler)->websocket_active = o_realloc(((struct _websocket_handler *)instance->websocket_handler)->websocket_active, (((struct _websocket_handler *)instance->websocket_handler)->nb_websocket_active-1)*sizeof(struct _websocket *));
          if (((struct _websocket_handler *)instance->websocket_handler)->websocket_active == NULL) {
            y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error allocating resources for instance->websocket_active");
            return U_ERROR_MEMORY;
          }
        } else {
          o_free(((struct _websocket_handler *)instance->websocket_handler)->websocket_active);
          ((struct _websocket_handler *)instance->websocket_handler)->websocket_active = NULL;
        }
        ((struct _websocket_handler *)instance->websocket_handler)->nb_websocket_active--;
        pthread_mutex_lock(&((struct _websocket_handler *)instance->websocket_handler)->websocket_close_lock);
        pthread_cond_broadcast(&((struct _websocket_handler *)instance->websocket_handler)->websocket_close_cond);
        pthread_mutex_unlock(&((struct _websocket_handler *)instance->websocket_handler)->websocket_close_lock);
        return U_OK;
      }
    }
    return U_ERROR_NOT_FOUND;
  } else {
    return U_ERROR_PARAMS;
  }
}

/********************************/
/** Common websocket functions **/
/********************************/

/**
 * Send a fragmented message in the websocket
 * each fragment size will be at most fragment_len
 * Return U_OK on success
 */
int ulfius_websocket_send_fragmented_message(struct _websocket_manager * websocket_manager,
                                             const uint8_t opcode,
                                             const uint64_t data_len,
                                             const char * data,
                                             const uint64_t fragment_len) {
  int ret = U_OK, ret_message, count = WEBSOCKET_MAX_CLOSE_TRY;
  struct _websocket_message * message;
  
  if (websocket_manager != NULL && websocket_manager->connected) {
    if (opcode == U_WEBSOCKET_OPCODE_CLOSE) {
      if (ulfius_send_websocket_message_managed(websocket_manager, U_WEBSOCKET_OPCODE_CLOSE, 0, NULL, 0) == U_OK) {
        // If message sent is U_WEBSOCKET_OPCODE_CLOSE, wait for the close response for WEBSOCKET_MAX_CLOSE_TRY messages max, then close the connection
        do {
          if (is_websocket_data_available(websocket_manager)) {
            message = NULL;
            ret_message = ulfius_read_incoming_message(websocket_manager, &message);
            if (ret_message == U_OK && message != NULL) {
              if (message->opcode == U_WEBSOCKET_OPCODE_CLOSE) {
                websocket_manager->connected = 0;
              }
              //if (ulfius_push_websocket_message(websocket_manager->message_list_incoming, message) != U_OK) {
              //  y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error pushing new websocket message in list");
              //}
            } else {
              websocket_manager->connected = 0;
            }
          }
        } while (websocket_manager->connected && (count-- > 0));
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error sending U_WEBSOCKET_OPCODE_CLOSE message");
      }
      websocket_manager->connected = 0;
    } else {
      ret = ulfius_send_websocket_message_managed(websocket_manager, opcode, data_len, data, fragment_len);
    }
  } else {
    ret = U_ERROR_PARAMS;
  }
  return ret;
}

/**
 * Send a message in the websocket
 * Return U_OK on success
 */
int ulfius_websocket_send_message(struct _websocket_manager * websocket_manager,
                                  const uint8_t opcode,
                                  const uint64_t data_len,
                                  const char * data) {
  return ulfius_websocket_send_fragmented_message(websocket_manager, opcode, data_len, data, data_len);
}

/**
 * Return the first message of the message list
 * Return NULL if message_list has no message
 * Returned value must be cleared after use
 */
struct _websocket_message * ulfius_websocket_pop_first_message(struct _websocket_message_list * message_list) {
  size_t len;
  struct _websocket_message * message = NULL;
  if (message_list != NULL && message_list->len > 0) {
    message = message_list->list[0];
    for (len=0; len < message_list->len-1; len++) {
      message_list->list[len] = message_list->list[len+1];
    }
    if (message_list->len > 1) {
      message_list->list = o_realloc(message_list->list, (message_list->len-1));
    } else {
      o_free(message_list->list);
      message_list->list = NULL;
    }
    message_list->len--;
  }
  return message;
}

/**
 * Clear data of a websocket message
 */
void ulfius_clear_websocket_message(struct _websocket_message * message) {
  if (message != NULL) {
    o_free(message->data);
    o_free(message);
  }
}

/************************************/
/** Init/clear websocket functions **/
/************************************/

/**
 * Clear all data related to the websocket
 */
int ulfius_clear_websocket(struct _websocket * websocket) {
  if (websocket != NULL) {
    if (websocket->websocket_manager != NULL &&
        websocket->urh != NULL &&
        websocket->websocket_manager->type == U_WEBSOCKET_SERVER &&
        MHD_upgrade_action (websocket->urh, MHD_UPGRADE_ACTION_CLOSE) != MHD_YES) {
      y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error sending MHD_UPGRADE_ACTION_CLOSE frame to urh");
    }
    if (websocket->instance != NULL) {
      ulfius_instance_remove_websocket_active(websocket->instance, websocket);
    }
    ulfius_clean_request_full(websocket->request);
    websocket->request = NULL;
    ulfius_clear_websocket_manager(websocket->websocket_manager);
    o_free(websocket->websocket_manager);
    websocket->websocket_manager = NULL;
    o_free(websocket);
    return U_OK;
  } else {
    return U_ERROR_PARAMS;
  }
}

/**
 * Clear data of a websocket message list
 */
void ulfius_clear_websocket_message_list(struct _websocket_message_list * message_list) {
  size_t i;
  if (message_list != NULL) {
    for (i=0; i < message_list->len; i++) {
      ulfius_clear_websocket_message(message_list->list[i]);
    }
    o_free(message_list->list);
  }
}

/**
 * Initialize a struct _websocket
 * return U_OK on success
 */
int ulfius_init_websocket(struct _websocket * websocket) {
  if (websocket != NULL) {
    websocket->instance = NULL;
    websocket->request = NULL;
    websocket->websocket_manager_callback = NULL;
    websocket->websocket_manager_user_data = NULL;
    websocket->websocket_incoming_message_callback = NULL;
    websocket->websocket_incoming_user_data = NULL;
    websocket->websocket_onclose_callback = NULL;
    websocket->websocket_onclose_user_data = NULL;
    websocket->websocket_manager = o_malloc(sizeof(struct _websocket_manager));
    websocket->urh = NULL;
    if (websocket->websocket_manager == NULL) {
      y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error allocating resources for websocket_manager");
      return U_ERROR_MEMORY;
    } else {
      websocket->websocket_manager->tls = 0;
      if (ulfius_init_websocket_manager(websocket->websocket_manager) != U_OK) {
        o_free(websocket->websocket_manager);
        y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error ulfius_init_websocket_manager");
        return U_ERROR;
      } else {
        return U_OK;
      }
    }
  } else {
    return U_ERROR_PARAMS;
  }
}

/**
 * Initialize a struct _websocket_manager
 * return U_OK on success
 */
int ulfius_init_websocket_manager(struct _websocket_manager * websocket_manager) {
  pthread_mutexattr_t mutexattr;
  int ret = U_OK;
  
  if (websocket_manager != NULL) {
    websocket_manager->connected = 0;
    websocket_manager->close_flag = 0;
    websocket_manager->mhd_sock = 0;
    websocket_manager->tcp_sock = 0;
    websocket_manager->protocol = NULL;
    websocket_manager->extensions = NULL;
    pthread_mutexattr_init ( &mutexattr );
    pthread_mutexattr_settype( &mutexattr, PTHREAD_MUTEX_RECURSIVE );
    if (pthread_mutex_init(&(websocket_manager->read_lock), &mutexattr) != 0 || pthread_mutex_init(&(websocket_manager->write_lock), &mutexattr) != 0) {
      y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Impossible to initialize Mutex Lock for websocket");
      ret = U_ERROR;
    } else if (pthread_mutex_init(&websocket_manager->status_lock, NULL) || pthread_cond_init(&websocket_manager->status_cond, NULL)) {
      y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error initializing status_lock or status_cond");
      ret = U_ERROR;
    } else if ((websocket_manager->message_list_incoming = o_malloc(sizeof(struct _websocket_message_list))) == NULL ||
               ulfius_init_websocket_message_list(websocket_manager->message_list_incoming) != U_OK ||
               (websocket_manager->message_list_outcoming = o_malloc(sizeof(struct _websocket_message_list))) == NULL ||
               ulfius_init_websocket_message_list(websocket_manager->message_list_outcoming) != U_OK) {
      y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error initializing message_list_incoming or message_list_outcoming");
      ret = U_ERROR_MEMORY;
    }
    websocket_manager->fds.events = POLLIN | POLLRDHUP;
    websocket_manager->type = U_WEBSOCKET_NONE;

    if (ret != U_OK) {
      o_free(websocket_manager->message_list_incoming);
      o_free(websocket_manager->message_list_outcoming);
    }
    pthread_mutexattr_destroy(&mutexattr);
  } else {
    ret = U_ERROR_PARAMS;
  }
  return ret;
}

/**
 * Clear data of a websocket_manager
 */
void ulfius_clear_websocket_manager(struct _websocket_manager * websocket_manager) {
  if (websocket_manager != NULL) {
    pthread_mutex_destroy(&websocket_manager->read_lock);
    pthread_mutex_destroy(&websocket_manager->write_lock);
    ulfius_clear_websocket_message_list(websocket_manager->message_list_incoming);
    o_free(websocket_manager->message_list_incoming);
    websocket_manager->message_list_incoming = NULL;
    ulfius_clear_websocket_message_list(websocket_manager->message_list_outcoming);
    o_free(websocket_manager->message_list_outcoming);
    websocket_manager->message_list_outcoming = NULL;
    o_free(websocket_manager->protocol);
    o_free(websocket_manager->extensions);
  }
}

/********************************/
/** Server websocket functions **/
/********************************/

/**
 * Set a websocket in the response
 * You must set at least websocket_manager_callback or websocket_incoming_message_callback
 * @Parameters
 * response: struct _u_response to send back the websocket initialization, mandatory
 * websocket_protocol: list of protocols, separated by a comma, or NULL if all protocols are accepted
 * websocket_extensions: list of extensions, separated by a comma, or NULL if all extensions are accepted
 * websocket_manager_callback: callback function called right after the handshake acceptance, optional
 * websocket_manager_user_data: any data that will be given to the websocket_manager_callback, optional
 * websocket_incoming_message_callback: callback function called on each incoming complete message, optional
 * websocket_incoming_user_data: any data that will be given to the websocket_incoming_message_callback, optional
 * websocket_onclose_callback: callback function called right before closing the websocket, must be complete for the websocket to close
 * websocket_onclose_user_data: any data that will be given to the websocket_onclose_callback, optional
 * @Return value: U_OK on success
 */
int ulfius_set_websocket_response(struct _u_response * response,
                                   const char * websocket_protocol,
                                   const char * websocket_extensions, 
                                   void (* websocket_manager_callback) (const struct _u_request * request,
                                                                       struct _websocket_manager * websocket_manager,
                                                                       void * websocket_manager_user_data),
                                   void * websocket_manager_user_data,
                                   void (* websocket_incoming_message_callback) (const struct _u_request * request,
                                                                                struct _websocket_manager * websocket_manager,
                                                                                const struct _websocket_message * message,
                                                                                void * websocket_incoming_user_data),
                                   void * websocket_incoming_user_data,
                                   void (* websocket_onclose_callback) (const struct _u_request * request,
                                                                       struct _websocket_manager * websocket_manager,
                                                                       void * websocket_onclose_user_data),
                                   void * websocket_onclose_user_data) {
  if (response != NULL && (websocket_manager_callback != NULL || websocket_incoming_message_callback)) {
    if (((struct _websocket_handle *)response->websocket_handle)->websocket_protocol != NULL) {
      o_free(((struct _websocket_handle *)response->websocket_handle)->websocket_protocol);
    }
    ((struct _websocket_handle *)response->websocket_handle)->websocket_protocol = o_strdup(websocket_protocol);
    if (((struct _websocket_handle *)response->websocket_handle)->websocket_protocol == NULL && websocket_protocol != NULL) {
      y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error allocating resources for response->websocket_protocol");
      return U_ERROR_MEMORY;
    }
    if (((struct _websocket_handle *)response->websocket_handle)->websocket_extensions != NULL) {
      o_free(((struct _websocket_handle *)response->websocket_handle)->websocket_extensions);
    }
    ((struct _websocket_handle *)response->websocket_handle)->websocket_extensions = o_strdup(websocket_extensions);
    if (((struct _websocket_handle *)response->websocket_handle)->websocket_extensions == NULL && websocket_extensions != NULL) {
      y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error allocating resources for response->websocket_extensions");
      o_free(((struct _websocket_handle *)response->websocket_handle)->websocket_protocol);
      return U_ERROR_MEMORY;
    }
    ((struct _websocket_handle *)response->websocket_handle)->websocket_manager_callback = websocket_manager_callback;
    ((struct _websocket_handle *)response->websocket_handle)->websocket_manager_user_data = websocket_manager_user_data;
    ((struct _websocket_handle *)response->websocket_handle)->websocket_incoming_message_callback = websocket_incoming_message_callback;
    ((struct _websocket_handle *)response->websocket_handle)->websocket_incoming_user_data = websocket_incoming_user_data;
    ((struct _websocket_handle *)response->websocket_handle)->websocket_onclose_callback = websocket_onclose_callback;
    ((struct _websocket_handle *)response->websocket_handle)->websocket_onclose_user_data = websocket_onclose_user_data;
    return U_OK;
  } else {
    return U_ERROR_PARAMS;
  }
}

/**
 * Sets the websocket in closing mode
 * The websocket will not necessarily be closed at the return of this function,
 * it will process through the end of the `websocket_manager_callback`
 * and the `websocket_onclose_callback` calls first.
 * return U_OK on success
 * or U_ERROR on error
 */
int ulfius_websocket_send_close_signal(struct _websocket_manager * websocket_manager) {
  if (websocket_manager != NULL) {
    websocket_manager->close_flag = 1;
    return U_OK;
  } else {
    return U_ERROR_PARAMS;
  }
}

/**
 * Returns the status of the websocket connection
 * Returned values can be U_WEBSOCKET_STATUS_OPEN or U_WEBSOCKET_STATUS_CLOSE
 * wether the websocket is open or closed, or U_WEBSOCKET_STATUS_ERROR on error
 */
int ulfius_websocket_status(struct _websocket_manager * websocket_manager) {
  if (websocket_manager != NULL) {
    return websocket_manager->connected?U_WEBSOCKET_STATUS_OPEN:U_WEBSOCKET_STATUS_CLOSE;
  } else {
    return U_WEBSOCKET_STATUS_ERROR;
  }
}

/**
 * Wait until the websocket connection is closed or the timeout in milliseconds is reached
 * if timeout is 0, no timeout is set
 * Returned values can be U_WEBSOCKET_STATUS_OPEN or U_WEBSOCKET_STATUS_CLOSE
 * wether the websocket is open or closed, or U_WEBSOCKET_STATUS_ERROR on error
 */
int ulfius_websocket_wait_close(struct _websocket_manager * websocket_manager, unsigned int timeout) {
  struct timespec abstime;
  int ret;
  
  if (websocket_manager != NULL) {
    if (websocket_manager->connected) {
      if (timeout) {
        clock_gettime(CLOCK_REALTIME, &abstime);
        abstime.tv_nsec += ((timeout%1000) * 1000000);
        abstime.tv_sec += (timeout / 1000);
        pthread_mutex_lock(&websocket_manager->status_lock);
        ret = pthread_cond_timedwait(&websocket_manager->status_cond, &websocket_manager->status_lock, &abstime);
        pthread_mutex_unlock(&websocket_manager->status_lock);
        return ((ret == ETIMEDOUT && websocket_manager->connected)?U_WEBSOCKET_STATUS_OPEN:U_WEBSOCKET_STATUS_CLOSE);
      } else {
        pthread_mutex_lock(&websocket_manager->status_lock);
        pthread_cond_wait(&websocket_manager->status_cond, &websocket_manager->status_lock);
        pthread_mutex_unlock(&websocket_manager->status_lock);
        return U_WEBSOCKET_STATUS_CLOSE;
      }
    } else {
      return U_WEBSOCKET_STATUS_CLOSE;
    }
  } else {
    return U_WEBSOCKET_STATUS_ERROR;
  }
}

/********************************/
/** Client websocket functions **/
/********************************/

static long random_at_most(long max) {
  unsigned char
  num_bins = (unsigned char) max + 1,
  num_rand = (unsigned char) 0xff,
  bin_size = num_rand / num_bins,
  defect   = num_rand % num_bins;

  unsigned char x[1];
  do {
    gnutls_rnd(GNUTLS_RND_KEY, x, sizeof(x));
  }
  // This is carefully written not to overflow
  while (num_rand - defect <= (unsigned char)x[0]);

  // Truncated division is intentional
  return x[0]/bin_size;
}

/**
 * Generates a random string and store it in str
 */
static char * rand_string(char * str, size_t str_size) {
  const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  size_t n;
  
  if (str_size > 0 && str != NULL) {
    for (n = 0; n < str_size; n++) {
      long key = random_at_most((sizeof(charset)) - 2);
      str[n] = charset[key];
    }
    str[str_size] = '\0';
    return str;
  } else {
    return NULL;
  }
}

/**
 * Set values for a struct _u_request to open a websocket
 * request must be previously initialized
 * Return U_OK on success
 */
int ulfius_set_websocket_request(struct _u_request * request,
                                  const char * url,
                                  const char * websocket_protocol,
                                  const char * websocket_extensions) {
  int ret;
  char rand_str[17] = {0}, rand_str_base64[25] = {0};
  size_t out_len;
  
  if (request != NULL && url != NULL) {
    o_free(request->http_protocol);
    o_free(request->http_verb);
    o_free(request->http_url);
    request->http_protocol = o_strdup("1.1");
    request->http_verb = o_strdup("GET");
    request->http_url = o_strdup(url);
    if (websocket_protocol != NULL) {
      u_map_put(request->map_header, "Sec-WebSocket-Protocol", websocket_protocol);
    }
    if (websocket_extensions != NULL) {
      u_map_put(request->map_header, "Sec-WebSocket-Extensions", websocket_extensions);
    }
    u_map_put(request->map_header, "Sec-WebSocket-Version", "13");
    u_map_put(request->map_header, "Upgrade", "websocket");
    u_map_put(request->map_header, "Connection", "Upgrade");
    u_map_put(request->map_header, "Content-Length", "0");
    u_map_put(request->map_header, "User-Agent", U_WEBSOCKET_USER_AGENT "/" STR(ULFIUS_VERSION));
    rand_string(rand_str, 16);
    if (!o_base64_encode((unsigned char *)rand_str, 16, (unsigned char *)rand_str_base64, &out_len)) {
      y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error o_base64_encode with the input string %s", rand_str);
      ret = U_ERROR;
    } else {
      u_map_put(request->map_header, "Sec-WebSocket-Key", rand_str_base64);
      ret = U_OK;
    }
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error ulfius_set_websocket_request input parameters");
    ret = U_ERROR;
  }
  return ret;
}

/**
 * Open a websocket client connection
 * Return U_OK on success
 */
int ulfius_open_websocket_client_connection(struct _u_request * request,
                                            void (* websocket_manager_callback) (const struct _u_request * request,
                                                                                 struct _websocket_manager * websocket_manager,
                                                                                 void * websocket_manager_user_data),
                                            void * websocket_manager_user_data,
                                            void (* websocket_incoming_message_callback) (const struct _u_request * request,
                                                                                          struct _websocket_manager * websocket_manager,
                                                                                          const struct _websocket_message * message,
                                                                                          void * websocket_incoming_user_data),
                                            void * websocket_incoming_user_data,
                                            void (* websocket_onclose_callback) (const struct _u_request * request,
                                                                                 struct _websocket_manager * websocket_manager,
                                                                                 void * websocket_onclose_user_data),
                                            void * websocket_onclose_user_data,
                                            struct _websocket_client_handler * websocket_client_handler,
                                            struct _u_response * response) {
  int ret;
  struct yuarel y_url;
  char * url, * basic_auth_encoded_header, * basic_auth, * basic_auth_encoded;
  size_t basic_auth_encoded_len;
  struct _websocket * websocket;
  pthread_t thread_websocket;
  int thread_ret_websocket = 0, thread_detach_websocket = 0;
  
  if (request != NULL && response != NULL && (websocket_manager_callback != NULL || websocket_incoming_message_callback != NULL)) {
    url = o_strdup(request->http_url);
    if (!yuarel_parse(&y_url, url)) {
      if (0 == o_strcasecmp("http", y_url.scheme) || 0 == o_strcasecmp("https", y_url.scheme) || 0 == o_strcasecmp("ws", y_url.scheme) || 0 == o_strcasecmp("wss", y_url.scheme)) {
        if (!y_url.port) {
          if (0 == o_strcasecmp("http", y_url.scheme) || 0 == o_strcasecmp("ws", y_url.scheme)) {
            y_url.port = 80;
          } else {
            y_url.port = 443;
          }
        }
        if (y_url.username != NULL && y_url.password != NULL) {
          basic_auth = msprintf("%s:%s", y_url.username, y_url.password);
          basic_auth_encoded = o_malloc((o_strlen(basic_auth)*4/3)+1);
          memset(basic_auth_encoded, 0, (o_strlen(basic_auth)*4/3)+1);
          if (o_base64_encode((const unsigned char *)basic_auth, o_strlen(basic_auth), (unsigned char *)basic_auth_encoded, &basic_auth_encoded_len)) {
            basic_auth_encoded_header = msprintf("Basic: %s", basic_auth_encoded);
            u_map_remove_from_key(request->map_header, "Authorization");
            u_map_put(request->map_header, "Authorization", basic_auth_encoded_header);
            o_free(basic_auth_encoded_header);
            o_free(basic_auth_encoded);
          } else {
            y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error o_base64_encode");
          }
          o_free(basic_auth);
        }
        
        websocket = o_malloc(sizeof(struct _websocket));
        if (websocket != NULL && ulfius_init_websocket(websocket) == U_OK) {
          websocket->request = ulfius_duplicate_request(request);
          websocket->websocket_manager->type = U_WEBSOCKET_CLIENT;
          websocket->websocket_manager_callback = websocket_manager_callback;
          websocket->websocket_manager_user_data = websocket_manager_user_data;
          websocket->websocket_incoming_message_callback = websocket_incoming_message_callback;
          websocket->websocket_incoming_user_data = websocket_incoming_user_data;
          websocket->websocket_onclose_callback = websocket_onclose_callback;
          websocket->websocket_onclose_user_data = websocket_onclose_user_data;
          // Open connection
          if (0 == o_strcasecmp("http", y_url.scheme) || 0 == o_strcasecmp("ws", y_url.scheme)) {
            websocket->websocket_manager->tls = 0;
            if (ulfius_open_websocket(request, &y_url, websocket, response) != U_OK) {
              y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error ulfius_open_websocket");
              ulfius_clear_websocket(websocket);
              ret = U_ERROR;
            } else {
              ret = U_OK;
            }
          } else {
            websocket->websocket_manager->tls = 1;
            if (ulfius_open_websocket_tls(request, &y_url, websocket, response) != U_OK) {
              y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error ulfius_open_websocket_tls");
              ulfius_clear_websocket(websocket);
              ret = U_ERROR;
            } else {
              ret = U_OK;
            }
          }
          if (ret == U_OK) {
            thread_ret_websocket = pthread_create(&thread_websocket, NULL, ulfius_thread_websocket, (void *)websocket);
            thread_detach_websocket = pthread_detach(thread_websocket);
            if (thread_ret_websocket || thread_detach_websocket) {
              y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error creating or detaching websocket manager thread, return code: %d, detach code: %d",
                            thread_ret_websocket, thread_detach_websocket);
              ulfius_clear_websocket(websocket);
              ret = U_ERROR;
            }
            websocket_client_handler->websocket = websocket;
          }
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error allocating resources for websocket");
          ret = U_ERROR_MEMORY;
        }
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Unknown scheme, please use one of the following: 'http', 'https', 'ws', 'wss'");
        ret = U_ERROR_PARAMS;
      }
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error parsing url");
      ret = U_ERROR_PARAMS;
    }
    o_free(url);
  } else {
    ret = U_ERROR_PARAMS;
  }
  return ret;
}

/**
 * Send a close signal to the websocket
 * return U_OK when the signal is sent
 * or U_ERROR on error
 */
int ulfius_websocket_client_connection_send_close_signal(struct _websocket_client_handler * websocket_client_handler) {
  if (websocket_client_handler != NULL) {
    return ulfius_websocket_send_close_signal(websocket_client_handler->websocket->websocket_manager);
  } else {
    return U_ERROR_PARAMS;
  }
}

/**
 * Closes a websocket client connection
 * return U_OK when the websocket is closed
 * or U_ERROR on error
 */
int ulfius_websocket_client_connection_close(struct _websocket_client_handler * websocket_client_handler) {
  if (websocket_client_handler != NULL) {
    if (ulfius_websocket_send_close_signal(websocket_client_handler->websocket->websocket_manager) == U_OK) {
      if (ulfius_websocket_wait_close(websocket_client_handler->websocket->websocket_manager, 0) != U_WEBSOCKET_STATUS_CLOSE) {
        y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error ulfius_websocket_send_close_signal");
        return U_ERROR;
      }
      ulfius_clear_websocket(websocket_client_handler->websocket);
      return U_OK;
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "Ulfius - Error ulfius_websocket_send_close_signal");
      return U_ERROR;
    }
  } else {
    return U_ERROR_PARAMS;
  }
}

/**
 * Returns the status of the websocket client connection
 * Returned values can be U_WEBSOCKET_STATUS_OPEN or U_WEBSOCKET_STATUS_CLOSE
 * wether the websocket is open or closed, or U_WEBSOCKET_STATUS_ERROR on error
 */
int ulfius_websocket_client_connection_status(struct _websocket_client_handler * websocket_client_handler) {
  if (websocket_client_handler != NULL) {
    return ulfius_websocket_status(websocket_client_handler->websocket->websocket_manager);
  } else {
    return U_WEBSOCKET_STATUS_ERROR;
  }
}

/**
 * Wait until the websocket client connection is closed or the timeout in milliseconds is reached
 * if timeout is 0, no timeout is set
 * Returned values can be U_WEBSOCKET_STATUS_OPEN or U_WEBSOCKET_STATUS_CLOSE
 * wether the websocket is open or closed, or U_WEBSOCKET_STATUS_ERROR on error
 */
int ulfius_websocket_client_connection_wait_close(struct _websocket_client_handler * websocket_client_handler, unsigned int timeout) {
  int ret;
  
  if (websocket_client_handler != NULL) {
    ret = ulfius_websocket_wait_close(websocket_client_handler->websocket->websocket_manager, timeout);
    if (ret == U_WEBSOCKET_STATUS_CLOSE && websocket_client_handler->websocket != NULL) {
      ulfius_clear_websocket(websocket_client_handler->websocket);
    }
    return ret;
  } else {
    return U_WEBSOCKET_STATUS_ERROR;
  }
}

#endif
