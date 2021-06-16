/***************************************************************************//**
 * @file
 * @brief Co-Processor Communication Protocol (CPC) - UART driver
 * @version 3.2.0
 *******************************************************************************
 * # License
 * <b>Copyright 2021 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * The licensor of this software is Silicon Laboratories Inc. Your use of this
 * software is governed by the terms of Silicon Labs Master Software License
 * Agreement (MSLA) available at
 * www.silabs.com/about-us/legal/master-software-license-agreement. This
 * software is distributed to you in Source Code format and is governed by the
 * sections of the MSLA applicable to Source Code.
 *
 ******************************************************************************/

#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "log.h"
#include "utils.h"
#include "driver_uart.h"
#include "hdlc.h"
#include "crc.h"

#define UART_BUFFER_SIZE 4096 + SLI_CPC_HDLC_HEADER_RAW_SIZE
#define MAX_EPOLL_EVENTS 1

static int fd_uart;
static int fd_core;
static int fd_epoll;
static pthread_t drv_thread;

static unsigned int uart_bitrate;

typedef void (*driver_epoll_callback_t)(void);

static void* driver_thread_func(void* param);

static int driver_uart_open(const char *device, unsigned int bitrate, bool hardflow);

static void driver_uart_process_uart(void);
static void driver_uart_process_core(void);

/*
 * @return The number of bytes appended to the buffer
 */
static size_t read_and_append_uart_received_data(uint8_t *buffer, size_t buffer_head, size_t buffer_size);

/*
 * Call this function in loop over the buffer to delimit and push the frames to the core
 *
 * @return Whether or not this call has delimited a pushed a frame, in other words,
 *         shall this function be called again in a loop
 */
static bool delimit_and_push_frames_to_core(uint8_t *buffer, size_t *buffer_head);

/*
 * Insures the start of the buffer is aligned with the start of a valid checksum
 * and re-synch in case the buffer starts with garbage.
 */
static bool header_re_synch(uint8_t *buffer, size_t *buffer_head);

pthread_t driver_uart_init(int *fd_to_core, const char *device, unsigned int bitrate, bool hardflow)
{
  int fd_sockets[2];
  ssize_t ret;

  fd_uart = driver_uart_open(device, bitrate, hardflow);
  uart_bitrate = bitrate;

  /* Flush the uart IO fifo */

  tcflush(fd_uart, TCIOFLUSH);

  ret = socketpair(AF_UNIX, SOCK_DGRAM, 0, fd_sockets);
  FATAL_SYSCALL_ON(ret < 0);

  fd_core  = fd_sockets[0];

  *fd_to_core = fd_sockets[1];

  /* Setup epoll */
  {
    struct epoll_event event = {};

    /* Create the epoll set */
    {
      fd_epoll = epoll_create1(0);
      FATAL_SYSCALL_ON(fd_epoll < 0);
    }

    /* Setup the socket to the core */
    {
      event.events = EPOLLIN; /* Level-triggered read() availability */
      event.data.ptr = driver_uart_process_core;

      ret = epoll_ctl(fd_epoll, EPOLL_CTL_ADD, fd_core, &event);
      FATAL_SYSCALL_ON(ret < 0);
    }

    /* Setup the uart */
    {
      event.events = EPOLLIN; /* Level-triggered read() availability */
      event.data.ptr = driver_uart_process_uart;

      ret = epoll_ctl(fd_epoll, EPOLL_CTL_ADD, fd_uart, &event);
      FATAL_SYSCALL_ON(ret < 0);
    }
  }

  /* create driver thread */
  ret = pthread_create(&drv_thread, NULL, driver_thread_func, NULL);
  FATAL_ON(ret != 0);

  ret = pthread_setname_np(drv_thread, "drv_thread");
  FATAL_ON(ret != 0);

  TRACE_DRIVER("Opening uart file %s\n", device);

  TRACE_DRIVER("Init done");

  return drv_thread;
}

static void* driver_thread_func(void* param)
{
  (void) param;

  struct epoll_event events[MAX_EPOLL_EVENTS] = {};

  TRACE_DRIVER("Thread start");

  while (1) {
    int event_count;

    /* Wait for action */
    {
      do {
        event_count = epoll_wait(fd_epoll, events, MAX_EPOLL_EVENTS, -1);
      } while ((event_count == -1) && (errno == EINTR));

      FATAL_SYSCALL_ON(event_count < 0);

      /* Timeouts should not occur */
      FATAL_ON(event_count == 0);
    }

    /* Process each ready file descriptor*/
    {
      size_t event_i;
      for (event_i = 0; event_i != (size_t)event_count; event_i++) {
        driver_epoll_callback_t callback = (driver_epoll_callback_t) events[event_i].data.ptr;
        callback();
      }
    }
  } //while(1)
  return 0;
}

static int driver_uart_open(const char *device, unsigned int bitrate, bool hardflow)
{
  static const struct {
    unsigned int val;
    int symbolic;
  } conversion[] = {
    { 9600, B9600 },
    { 19200, B19200 },
    { 38400, B38400 },
    { 57600, B57600 },
    { 115200, B115200 },
    { 230400, B230400 },
    { 460800, B460800 },
    { 921600, B921600 },
  };
  struct termios tty;
  int sym_bitrate = -1;
  int fd;

  TRACE_DRIVER("Opening UART device");
  fd = open(device, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    FATAL("%s: %m", device);
  }

  TRACE_DRIVER("Configuring UART device");
  if (tcgetattr(fd, &tty) == -1) {
    FATAL("tcgetattr: %m");
  }
  size_t i;
  for (i = 0; i < ARRAY_SIZE(conversion); i++) {
    if (conversion[i].val == bitrate) {
      sym_bitrate = conversion[i].symbolic;
    }
  }
  if (sym_bitrate < 0) {
    FATAL("invalid bitrate: %d", bitrate);
  }
  cfsetispeed(&tty, (speed_t)sym_bitrate);
  cfsetospeed(&tty, (speed_t)sym_bitrate);
  cfmakeraw(&tty);
  /* Nonblocking read. */
  tty.c_cc[VTIME] = 0;
  tty.c_cc[VMIN] = 1;
  tty.c_iflag &= (unsigned) ~(IXON);
  tty.c_iflag &= (unsigned) ~(IXOFF);
  tty.c_iflag &= (unsigned) ~(IXANY);
  tty.c_cflag &= (unsigned) ~(HUPCL);
  tty.c_cflag |= CLOCAL;
  if (hardflow) {
    tty.c_cflag |= CRTSCTS;
  } else {
    tty.c_cflag &= ~CRTSCTS;
  }
  if (tcsetattr(fd, TCSAFLUSH, &tty) < 0) {
    FATAL("tcsetattr: %m");
  }
  return fd;
}

static void driver_uart_process_uart(void)
{
  static uint8_t buffer[UART_BUFFER_SIZE];
  static size_t buffer_head = 0;
  static enum {EXPECTING_HEADER, EXPECTING_PAYLOAD} state = EXPECTING_HEADER;

  /* Put the read data at the tip of the buffer head and increment it. */
  buffer_head += read_and_append_uart_received_data(buffer, buffer_head, sizeof(buffer));

  while (1) {
    switch (state) {
      case EXPECTING_HEADER:
        /* Synchronize the start of 'buffer' with the start of a valid header with valid checksum. */
        if (header_re_synch(buffer, &buffer_head)) {
          /* We are synchronized on a valid header, start delimiting the data that follows into a frame. */
          state = EXPECTING_PAYLOAD;
        } else {
          /* We went through all the data contained in 'buffer' and haven't synchronized on a header.
           * Go back to waiting for more data. */
          return;
        }
        break;

      case EXPECTING_PAYLOAD:
        if (delimit_and_push_frames_to_core(buffer, &buffer_head)) {
          /* A frame has been delimited and pushed to the core, go back to synchronizing on the next header */
          state = EXPECTING_HEADER;
        } else {
          /* Not yet enough data, go back to waiting. */
          return;
        }
        break;

      default:
        BUG();
        break;
    }
  }
}

/* Append UART new data to the frame delimiter processing buffer */
static size_t read_and_append_uart_received_data(uint8_t *buffer, size_t buffer_head, size_t buffer_size)
{
  size_t available_bytes;
  size_t available_space;
  size_t actual_read_bytes;
  uint8_t* temp_buffer;

  /* Poll the uart to get the available bytes */
  {
    int retval = ioctl(fd_uart, FIONREAD, &available_bytes);

    FATAL_SYSCALL_ON(retval < 0);

    /* The uart had no data. The epoll is supposed to wake us up when the uart has data. */
    BUG_ON(available_bytes == 0);
  }

  /* Make sure we don't read more data than the supplied buffer can handle */
  {
    available_space = buffer_size - buffer_head;

    if (available_bytes < available_space) {
      actual_read_bytes = available_bytes;
    } else {
      actual_read_bytes = available_space;
    }
  }

  /* Get a temporary buffer of the right size */
  {
    temp_buffer = (uint8_t*) malloc(actual_read_bytes);

    FATAL_ON(temp_buffer == NULL);
  }

  /* Read the uart data into the temp buffer */
  {
    ssize_t read_retval = read(fd_uart, temp_buffer, actual_read_bytes);

    FATAL_ON(read_retval < 0);

    FATAL_ON((size_t) read_retval != actual_read_bytes);
  }

  //TRACE_FRAME("Driver : received chunck from uart: ", temp_buffer, (size_t)actual_read_bytes);

  /* copy the data in the main buffer */
  memcpy(&buffer[buffer_head], temp_buffer, actual_read_bytes);

  free(temp_buffer);

  return actual_read_bytes;
}

static bool validate_header(uint8_t *header_start)
{
  uint16_t hcs;

  if (header_start[SLI_CPC_HDLC_FLAG_POS] != SLI_CPC_HDLC_FLAG_VAL) {
    return false;
  }

  hcs = hdlc_get_hcs(header_start);

  if (!sli_cpc_validate_crc_sw(header_start, SLI_CPC_HDLC_HEADER_SIZE, hcs)) {
    return false;
  }

  return true;
}

static bool header_re_synch(uint8_t *buffer, size_t *buffer_head)
{
  if (*buffer_head < SLI_CPC_HDLC_HEADER_RAW_SIZE) {
    /* There's not enough data for a header, nothing to re-synch */
    //TRACE_DRIVER("re-sync : not enough for a header");
    return false;
  }

  /* If we think of a header like a sliding window of width SLI_CPC_HDLC_HEADER_RAW_SIZE,
   * then we can slide it 'num_header_combination' times over the data. */
  const size_t num_header_combination = *buffer_head - SLI_CPC_HDLC_HEADER_RAW_SIZE + 1;

  TRACE_DRIVER("re-sync : Will test %i header combination", num_header_combination);

  size_t i;

  for (i = 0; i != num_header_combination; i++) {
    if (validate_header(&buffer[i])) {
      if (i == 0) {
        /* The start of the buffer is aligned with a good header, don't do anything */
        TRACE_DRIVER("re-sync : The start of the buffer is aligned with a good header");
      } else {
        /* We had 'i' number of bad bytes until we struck a good header, move back the data
         * to the beginning of the buffer */
        memmove(&buffer[0], &buffer[i], *buffer_head - i);

        /* We crushed 'i' bytes at the start of the buffer */
        *buffer_head -= i;
        TRACE_DRIVER("re-sync : had 'i' number of bad bytes until we struck a good header", i);
      }
      return true;
    } else {
      /* The header is not valid, continue until it is */
    }
  }

  /* If we land here, no header at all was found. Keep the last 'SLI_CPC_HDLC_HEADER_RAW_SIZE - 1' bytes and
   * bring them back at the start of the buffer so that the next appended byte could complete that potential header */
  {
    memmove(&buffer[0], &buffer[num_header_combination], SLI_CPC_HDLC_HEADER_RAW_SIZE - 1);

    *buffer_head = SLI_CPC_HDLC_HEADER_RAW_SIZE - 1;
  }

  return false;
}

/*
 * In this function, it is assumed that the start of the buffer 'buffer' is aligned with the
 * start of a header because each time this function delimits a frame, it moves back the
 * remaining data back to the start of the buffer. Except when things go wrong, the start
 * if the remaining data will be the start of a next header.
 */
static bool delimit_and_push_frames_to_core(uint8_t *buffer, size_t *buffer_head)
{
  uint16_t payload_len; /* The length of the payload, as retrieved from the header (including the checksum) */
  size_t frame_size; /* The whole size of the frame */

  /* if not enough bytes even for a header */
  if (*buffer_head < SLI_CPC_HDLC_HEADER_RAW_SIZE) {
    return false;
  }

  payload_len = hdlc_get_length(buffer);

  frame_size = payload_len + SLI_CPC_HDLC_HEADER_RAW_SIZE;

  /* Check if we have enough data for a full frame*/
  if (frame_size > *buffer_head) {
    return false;
  }

  /* Push to core */
  {
    TRACE_FRAME("Driver : Frame delimiter : push delimited frame to core : ", buffer, frame_size);

    ssize_t write_retval = write(fd_core, buffer, frame_size);
    FATAL_SYSCALL_ON(write_retval < 0);

    /* Error if write is not complete */
    FATAL_ON((size_t)write_retval != frame_size);
  }

  /* Move the remaining data back to the start of the buffer. */
  {
    const size_t remaining_bytes = *buffer_head - frame_size;

    memmove(buffer, &buffer[frame_size], remaining_bytes);

    /* Adjust the buffer_head now that we have modified the buffer's content */
    *buffer_head = remaining_bytes;
  }

  /* A complete frame has been delimited. A second round of parsing can be done. */
  return true;
}

static void driver_uart_process_core(void)
{
  uint8_t buffer[UART_BUFFER_SIZE];
  ssize_t read_retval;

  {
    read_retval = read(fd_core, buffer, sizeof(buffer));

    FATAL_SYSCALL_ON(read_retval < 0);
  }

  {
    int ret;
    int bytes_remaining;
    ret = ioctl(fd_uart, TIOCOUTQ, &bytes_remaining);

    FATAL_SYSCALL_ON(ret < 0);
    BUG_ON(bytes_remaining < 0);

    while (bytes_remaining != 0) {
      // byte per usec
      unsigned int time_usec = (unsigned int)(((double)(bytes_remaining * 8) / uart_bitrate) * 1000000);
      usleep(time_usec);
      ioctl(fd_uart, TIOCOUTQ, &bytes_remaining);
    }

    // Wait at least twenty bytes to cause an idle event on the bus
    // TODO: Find appropriate idle time
    // TODO: Will be removed once secondary supports unsegmented frames
    unsigned int time_usec = (unsigned int)(((double)(20 * 8) / uart_bitrate) * 1000000);
    usleep(time_usec);
  }

  {
    ssize_t write_retval = write(fd_uart, buffer, (size_t)read_retval);

    FATAL_SYSCALL_ON(write_retval < 0);

    /* Error if write is not complete */
    FATAL_ON((size_t)write_retval != (size_t)read_retval);
  }

  TRACE_FRAME("Driver : flushed frame to uart: ", buffer, (size_t)read_retval);
}