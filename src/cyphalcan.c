/****************************************************************************
 * nxp_bms/BMS_v1/src/cyphalcan.c
 *
 * BSD 3-Clause License
 *
 * Copyright 2020-2024 NXP
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/
#include <canard.h>
#include <canard_dsdl.h>

#include <sched.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include <net/if.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <nuttx/random.h>

#include <poll.h>

#include <nuttx/can.h>
#include <netpacket/can.h>

#include <semaphore.h>

#include "cyphalcan.h"
#include "cli.h"

#ifdef CANARD_VERSION_MAJOR
#    undef CANARD_VERSION_MAJOR
#endif
#ifdef CANARD_VERSION_MINOR
#    undef CANARD_VERSION_MINOR
#endif

#include "socketcan.h"
#include "o1heap.h"

#include "data.h"

#include "pnp.h"
#include "portid.h"

#define NUNAVUT_ASSERT
#define reg_drone_service_battery_Status_0_2_cell_voltages_ARRAY_CAPACITY_ 6U
#include "nunavut/support/serialization.h"
#include "reg/drone/physics/electricity/SourceTs_0_1.h"
#include "reg/drone/service/battery/Status_0_2.h"
#include "reg/drone/service/battery/Parameters_0_3.h"
#include "uavcan/node/Heartbeat_1_0.h"
#include "uavcan/node/GetInfo_1_0.h"
#include "legacy/equipment/power/BatteryInfo_1_0.h"

/****************************************************************************
 * Defines
 ****************************************************************************/
#ifndef SEM_OCCUPIED_ERROR
#    define SEM_OCCUPIED_ERROR 255
#endif

#define ONE_SEC_IN_NS 1000000000
#define MS_TO_NS_MULT 1000000

#define APP_NODE_NAME                                                                                        \
    "org.nxp.bms" //"org.uavcan.libcanardv1.nuttx.demo" //CONFIG_EXAMPLES_LIBCANARDV1_APP_NODE_NAME

#define CYPHAL_NODE_HEALTH_OK       0
#define CYPHAL_NODE_HEALTH_WARNING  1
#define CYPHAL_NODE_HEALTH_ERROR    2
#define CYPHAL_NODE_HEALTH_CRITICAL 3

#define CYPHAL_NODE_MODE_OPERATIONAL    0
#define CYPHAL_NODE_MODE_INITIALIZATION 1

#define CYPHAL_GET_NODE_INFO_RESPONSE_MAX_SIZE   ((3015 + 7) / 8)
#define CYPHAL_GET_NODE_INFO_DATA_TYPE_SIGNATURE 0xee468a8121c46a9e
#define CYPHAL_GET_NODE_INFO_DATA_TYPE_ID        1

#define UNIQUE_ID_LENGTH_BYTES 16

#define CYPHALCAN_DAEMON_PRIORITY   110
#define CYPHALCAN_DAEMON_STACK_SIZE 3100 // 4000
#define CAN_DEVICE                  "can0"

#define CELSIUS_TO_KELVIN       272.15
#define AMPERE_HOURS_TO_COULOMB 3600
#define WH_TO_JOULE             3600
#define LIPO_TECHNOLOGY         100

/****************************************************************************
 * Types
 ****************************************************************************/

/****************************************************************************
 * private data
 ****************************************************************************/
static int    efd;
struct file * gEventfp;
struct pollfd pfds[2];
static bool   gCyphalcanInitialized = false;

CanardRxSubscription heartbeat_subscription;
CanardRxSubscription my_subscription;

/* Arena for memory allocation, used by the library */

#define O1_HEAP_SIZE 4096 // CONFIG_EXAMPLES_LIBCANARDV1_NODE_MEM_POOL_SIZE

/* Temporary development CYPHAL topic service ID to publish/subscribe from */
#define PORT_ID    4421
#define TOPIC_SIZE 512

O1HeapInstance *my_allocator;

static bool g_canard_daemon_started;

static uint8_t my_message_transfer_id; // Must be static or heap-allocated to retain state between calls.

/****************************************************************************
 * private Functions declerations
 ****************************************************************************/
int32_t set_energy_source_port_id(uavcan_register_Value_1_0 *value);

uavcan_register_Value_1_0 get_energy_source_port_id(void);

int32_t set_battery_status_port_id(uavcan_register_Value_1_0 *value);

uavcan_register_Value_1_0 get_battery_status_port_id(void);

int32_t set_battery_parameter_port_id(uavcan_register_Value_1_0 *value);

uavcan_register_Value_1_0 get_battery_parameter_port_id(void);

int32_t set_battery_info_port_id(uavcan_register_Value_1_0 *value);

uavcan_register_Value_1_0 get_battery_info_port_id(void);

static int32_t cyphalcan_task_initialize(CanardInstance *ins, CanardSocketInstance *sock_ins);

//! @brief the CYPHALCAN deamon task
static int CYPHALCANTask(int argc, char *argv[]);

static void *memAllocate(CanardInstance *const ins, const size_t amount);

static void memFree(CanardInstance *const ins, void *const pointer);

uint64_t getMonotonicTimestampUSec(void);

static void EnergySourceToTransmitBuffer(CanardInstance *ins);

static void BatteryStatusToTransmitBuffer(CanardInstance *ins); //, uint64_t timestamp_usec);

static void BatteryParametersToTransmitBuffer(CanardInstance *ins);

static void BatteryInfoToTransmitBuffer(CanardInstance *ins);

// static void processReceivedTransfer(CanardTransfer *receive);

static bool processTxRxOnce(CanardInstance *ins, CanardSocketInstance *sock_ins, int timeout_msec);

/****************************************************************************
 * public functions
 ****************************************************************************/
/*!
 * @brief   this function initializes the CYPHAL CAN part
 *
 *          It will create the task to check and update the data
 *
 * @param   none
 *
 * @return  If successful, the function will return zero (OK). Otherwise, an error number will be returned to
 * indicate the error:
 *
 */
int cyphalcan_initialize(void)
{
    int ret = EXIT_SUCCESS;

    if(!gCyphalcanInitialized)
    {
        // if already started
        if(g_canard_daemon_started)
        {
            cli_printf("canard_main: receive and send task already running\n");
            return EXIT_SUCCESS;
        }

        ret = task_create(
            "CYHALCAN", CYPHALCAN_DAEMON_PRIORITY, CYPHALCAN_DAEMON_STACK_SIZE, CYPHALCANTask, NULL);

        if(ret < 0)
        {
            int errcode = errno;
            cli_printfError("cyphalcan_initialize ERROR: Failed to start CYHALCAN: %d\n", errcode);
            return EXIT_FAILURE;
        }
        else
        {
            ret = 0;
        }

        // remember it is initialized
        gCyphalcanInitialized = true;
    }

    return ret;
}

/*!
 * @brief   this function will increase the semaphore so the CYPHALCAN task will send the BMS status using CAN
 *
 * @return  If successful, the function will return zero (OK). Otherwise, an error number will be returned to
 * indicate the error:
 *
 */
int cyphalcan_sendBMSStatus(void)
{
    int ret = 0;

    if(!gCyphalcanInitialized)
    {
        ret = EXIT_FAILURE;
    }
    else
    {
        // signal pol to stop blocking
        if(cyphalcan_flushtx() > 0)
        {
            ret = 0;
        }
        else
        {
            ret = -1;
        }
    }

    // return to user
    return ret;
}

/****************************************************************************
 * private functions
 ****************************************************************************/

// TODO move this to a seperate file probably

int32_t set_energy_source_port_id(uavcan_register_Value_1_0 *value)
{
    if(uavcan_register_Value_1_0_is_natural16_(value) && value->natural16.value.count == 1)
    { // Natural 16
        // TODO check validity
        void *dataReturn;
        dataReturn = (int32_t *)data_setParameter(CYPHAL_ES_SUB_ID, &value->natural16.value.elements[0]);
        if(dataReturn == NULL)
        {
            return -CYPHAL_REGISTER_ERROR_SERIALIZATION;
        }
        return 0;
    }
    return -CYPHAL_REGISTER_ERROR_SERIALIZATION;
}

uavcan_register_Value_1_0 get_energy_source_port_id(void)
{
    void *                    dataReturn;
    uavcan_register_Value_1_0 value;

    dataReturn = (int32_t *)data_getParameter(CYPHAL_ES_SUB_ID, &value.natural16.value.elements[0], NULL);
    if(dataReturn == NULL)
    {
        value._tag_ = 0; // Empty
    }
    value.natural16.value.count = 1;
    value._tag_                 = 10; // TODO does nunavut generate ENUM/defines for this??
    return value;
}

int32_t set_battery_status_port_id(uavcan_register_Value_1_0 *value)
{
    if(uavcan_register_Value_1_0_is_natural16_(value) && value->natural16.value.count == 1)
    { // Natural 16
        // TODO check validity
        void *dataReturn;
        dataReturn = (int32_t *)data_setParameter(CYPHAL_BS_SUB_ID, &value->natural16.value.elements[0]);
        if(dataReturn == NULL)
        {
            return -CYPHAL_REGISTER_ERROR_SERIALIZATION;
        }
        return 0;
    }
    return -CYPHAL_REGISTER_ERROR_SERIALIZATION;
}

uavcan_register_Value_1_0 get_battery_status_port_id(void)
{
    void *                    dataReturn;
    uavcan_register_Value_1_0 value;

    dataReturn = (int32_t *)data_getParameter(CYPHAL_BS_SUB_ID, &value.natural16.value.elements[0], NULL);
    if(dataReturn == NULL)
    {
        value._tag_ = 0; // Empty
    }
    value.natural16.value.count = 1;
    value._tag_                 = 10; // TODO does nunavut generate ENUM/defines for this??
    return value;
}

int32_t set_battery_parameter_port_id(uavcan_register_Value_1_0 *value)
{
    if(uavcan_register_Value_1_0_is_natural16_(value) && value->natural16.value.count == 1)
    { // Natural 16
        // TODO check validity
        void *dataReturn;
        dataReturn = (int32_t *)data_setParameter(CYPHAL_BP_SUB_ID, &value->natural16.value.elements[0]);
        if(dataReturn == NULL)
        {
            return -CYPHAL_REGISTER_ERROR_SERIALIZATION;
        }
        return 0;
    }
    return -CYPHAL_REGISTER_ERROR_SERIALIZATION;
}

uavcan_register_Value_1_0 get_battery_parameter_port_id(void)
{
    void *                    dataReturn;
    uavcan_register_Value_1_0 value;

    dataReturn = (int32_t *)data_getParameter(CYPHAL_BP_SUB_ID, &value.natural16.value.elements[0], NULL);
    if(dataReturn == NULL)
    {
        value._tag_ = 0; // Empty
    }
    value.natural16.value.count = 1;
    value._tag_                 = 10; // TODO does nunavut generate ENUM/defines for this??
    return value;
}

int32_t set_battery_info_port_id(uavcan_register_Value_1_0 *value)
{
    if(uavcan_register_Value_1_0_is_natural16_(value) && value->natural16.value.count == 1)
    { // Natural 16
        // TODO check validity
        void *dataReturn;
        dataReturn =
            (int32_t *)data_setParameter(CYPHAL_LEGACY_BI_SUB_ID, &value->natural16.value.elements[0]);
        if(dataReturn == NULL)
        {
            return -CYPHAL_REGISTER_ERROR_SERIALIZATION;
        }
        return 0;
    }
    return -CYPHAL_REGISTER_ERROR_SERIALIZATION;
}

uavcan_register_Value_1_0 get_battery_info_port_id(void)
{
    void *                    dataReturn;
    uavcan_register_Value_1_0 value;

    dataReturn =
        (int32_t *)data_getParameter(CYPHAL_LEGACY_BI_SUB_ID, &value.natural16.value.elements[0], NULL);
    if(dataReturn == NULL)
    {
        value._tag_ = 0; // Empty
    }
    value.natural16.value.count = 1;
    value._tag_                 = 10; // TODO does nunavut generate ENUM/defines for this??
    return value;
}

static int32_t cyphalcan_task_initialize(CanardInstance *ins, CanardSocketInstance *sock_ins)
{
    uint8_t can_fd = 0;
    uint8_t nodeID;
    void *  dataReturn;
    int32_t canBitrate, canFdBitrate;
    void *  memoryPool;

    // cli_printf("cyphalcan_task_initialize\n");
    memoryPool = memalign(O1HEAP_ALIGNMENT, O1_HEAP_SIZE);

    if(memoryPool == NULL)
    {
        cli_printfError(
            "CYPHALCANTask ERROR: canard_daemon: memory pool allocation size %i failed\n", O1_HEAP_SIZE);
        return -2;
    }

    my_allocator = o1heapInit(memoryPool, O1_HEAP_SIZE, NULL, NULL);

    if(my_allocator == NULL)
    {
        cli_printfError("CYPHALCANTask ERROR: o1heapInit failed with size %d\n", O1_HEAP_SIZE);
        return -2;
    }

    *ins = canardInit(&memAllocate, &memFree);

    // get the node ID
    dataReturn = (int32_t *)data_getParameter(CAN_FD_MODE, &can_fd, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set the default value
        can_fd = CAN_FD_MODE_DEFAULT;

        cli_printfError("CYPHALCANTask ERROR: couldn't get canfd mode! setting default\n");
    }

    // mask the variable to be sure
    can_fd &= 1;

    // check if CAN FD is used
    if(can_fd)
    {
        ins->mtu_bytes = CANARD_MTU_CAN_FD;
    }
    else
    {
        ins->mtu_bytes = CANARD_MTU_CAN_CLASSIC;
    }

    // get the node ID
    dataReturn = (int32_t *)data_getParameter(CYPHAL_NODE_STATIC_ID, &nodeID, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set the default value
        nodeID = CYPHAL_NODE_STATIC_ID_DEFAULT;

        cli_printfError("CYPHALCANTask ERROR: couldn't get node id! setting default\n");
    }

    /* Open the CAN device for reading */
    socketcanOpen(sock_ins, CAN_DEVICE, can_fd);

    // get the bitrates
    dataReturn = (int32_t *)data_getParameter(CAN_BITRATE, &canBitrate, NULL);
    // check for error
    if(dataReturn == NULL)
    {
        // set the default value
        canBitrate = CAN_BITRATE_DEFAULT;

        cli_printfError("CYPHALCANTask ERROR: couldn't get canBitrate! setting default\n");
    }

    // get the CAN FD bitrate
    dataReturn = (int32_t *)data_getParameter(CAN_FD_BITRATE, &canFdBitrate, NULL);
    // check for error
    if(dataReturn == NULL)
    {
        // set the default value
        canFdBitrate = CAN_FD_BITRATE_DEFAULT;

        cli_printfError("CYPHALCANTask ERROR: couldn't get canFdBitrate! setting default\n");
    }

    // set the bitrates
    if(socketcanSetBitrate(sock_ins, CAN_DEVICE, canBitrate, canFdBitrate))
    {
        cli_printfError("CYPHALCANTask ERROR: couldn't set bitrates!\n");
    }

    if(sock_ins->s < 0)
    {
        cli_printfError("CYPHALCANTask ERROR: canard_daemon: ERROR: open %s failed: %d\n", CAN_DEVICE, errno);
        return -2;
    }

    // Setup pollfd for socket
    pfds[0].fd     = sock_ins->s;
    pfds[0].events = POLLIN;

    if(nodeID == CANARD_NODE_ID_UNSET)
    {
        // PNP is enabled
        cli_printf("CYPHALCANTask: CANARD_NODE_ID_UNSET\n");

        uint8_t unique_id[16];
        data_getUniqueid((uintptr_t)&unique_id[0], sizeof(unique_id));

        initPNPAllocatee(ins, unique_id);

        uint32_t random_no;
        random_no = ((float)rand() / RAND_MAX) * (1000000);

        uint64_t next_alloc_req = getMonotonicTimestampUSec() + random_no;

        cli_printf("CYPHALCANTask: Trying to get NODE ID\n");

        while(ins->node_id == CANARD_NODE_ID_UNSET)
        {
            // process the TX and RX buffer
            processTxRxOnce(ins, sock_ins, 10); // 10Ms

            const uint64_t ts = getMonotonicTimestampUSec();

            if(ts >= next_alloc_req)
            {
                next_alloc_req += ((float)rand() / RAND_MAX) * (1000000);
                int32_t result = PNPAllocRequest(ins);
                if(result)
                {
                    ins->node_id = PNPGetNodeID();
                }
            }
        }
    }
    else
    {
        ins->node_id = nodeID; // Static preconfigured nodeID
    }

    // cli_printf("canard_daemon: canard initialized\n");
    cli_printf(
        "CYPHALCANTask: start node (ID: %d Name: %s MTU: %d)\n", ins->node_id, APP_NODE_NAME, ins->mtu_bytes);

    // Init CYPHAL CAN register interfaces
    uavcan_node_GetInfo_Response_1_0 node_information; // TODO ADD INFO
    cyphal_register_interface_init(ins, &node_information);

    // tell the cyphal register interface that this register is usable
    // so the subject id (port id) can be set and get using this function
    cyphal_register_interface_add_entry(
        "energy_source", set_energy_source_port_id, get_energy_source_port_id);
    cyphal_register_interface_add_entry(
        "battery_status", set_battery_status_port_id, get_battery_status_port_id);
    cyphal_register_interface_add_entry(
        "battery_parameters", set_battery_parameter_port_id, get_battery_parameter_port_id);
    cyphal_register_interface_add_entry("battery_info", set_battery_info_port_id, get_battery_info_port_id);

    (void)canardRxSubscribe(ins, // Subscribe to messages uavcan.node.Heartbeat.
        CanardTransferKindMessage,
        32085, // The fixed Subject-ID of the Heartbeat message type (see DSDL definition).
        7,     // The maximum payload size (max DSDL object size) from the DSDL definition.
        CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC, &heartbeat_subscription);

    (void)canardRxSubscribe(ins, CanardTransferKindMessage,
        PORT_ID,    // The Service-ID to subscribe to.
        TOPIC_SIZE, // The maximum payload size (max DSDL object size).
        CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC, &my_subscription);

    return 0;
}

/****************************************************************************
 * Name: CYPHALCANTask
 *
 * Description:
 *
 ****************************************************************************/

static int CYPHALCANTask(int argc, char *argv[])
{
    CanardInstance       ins;
    CanardSocketInstance sock_ins;
    uint16_t             countBS = 10000, countBP = 10000;
    void *               dataReturn;
    uint16_t             t_meas;
    bool                 publish = false;

    // initialize eventfd to signal select while reading
    if((efd = eventfd(0, 0)) < 0)
    {
        // output to user
        cli_printfError("CYPHALCANTask ERROR: Couldn't initialize eventfd!\n");

        // return to the user
        return -1;
    }

    // Setup pollfd for eventfd
    pfds[1].fd     = efd;
    pfds[1].events = POLLIN;

    if(fs_getfilep(efd, &gEventfp) < 0)
    {
        cli_printfError("CYPHALCANTask ERROR: Couldn't initialize gEventfp!\n");
    }

    // wait for the first cyphalcan_sendBMSStatus() to continue initialize
    // wait for the bms application to trigger, before continuing 
    while(1)
    {
        // the event was triggered from the BMS application (cyphalcan_sendBMSStatus())
        if((poll(&pfds[1], 1, -1) > 0) && pfds[1].revents & POLLIN)
        {
            // break the while looop to continue
            break;
        }
    }

    // initialize the task and check if ok
    if(cyphalcan_task_initialize(&ins, &sock_ins) == 0)
    {
        // if the deamon is started
        g_canard_daemon_started = true;

        // loop endlessly
        for(;;)
        {
            // check if the BMS would like to publish BMS data
            if(publish)
            {
                // get the measurment time
                dataReturn = (int32_t *)data_getParameter(T_MEAS, &t_meas, NULL);

                // check for error
                if(dataReturn == NULL)
                {
                    // set the default value
                    t_meas = T_MEAS_DEFAULT;

                    // error output
                    cli_printfError("CYPHALCANTask ERROR: could not get t-meas!\n");
                }

                // make the energy source message
                EnergySourceToTransmitBuffer(&ins);

                // check if at least 1 seconds is passed
                if(++countBS >= (1000 / t_meas))
                {
                    // make the battery status message
                    BatteryStatusToTransmitBuffer(&ins);

                    // make the legacy battery info message
                    // will only send if subject ID != UINT16_MAX
                    BatteryInfoToTransmitBuffer(&ins);

                    // reset count
                    countBS = 0;
                }

                // check if the 5 seconds have passed
                if(++countBP >= (5000 / t_meas))
                {
                    // make the battery parameter message
                    BatteryParametersToTransmitBuffer(&ins);

                    // reset count
                    countBP = 0;
                }
            }

            // process the TX and RX buffer
            // and check if you want to publish the BMS data
            publish = processTxRxOnce(&ins, &sock_ins, 4000);
        }
    }

    // terminate the deamon
    g_canard_daemon_started = false;
    cli_printfWarning("canard_daemon: Terminating!\n");
    fflush(stdout);
    return -1;
}

/****************************************************************************
 * Name: memAllocate
 *
 * Description:
 *
 ****************************************************************************/
static void *memAllocate(CanardInstance *const ins, const size_t amount)
{
    (void)ins;
    return o1heapAllocate(my_allocator, amount);
}

/****************************************************************************
 * Name: memFree
 *
 * Description:
 *
 ****************************************************************************/

static void memFree(CanardInstance *const ins, void *const pointer)
{
    (void)ins;
    o1heapFree(my_allocator, pointer);
}


/****************************************************************************
 * Name: EnergySourceToTransmitBuffer
 *
 * Description:
 *   This function is called at 1 Hz rate from the main loop.
 *
 ****************************************************************************/

void EnergySourceToTransmitBuffer(CanardInstance *ins)
{
    void *   dataReturn;
    uint8_t  statusFlagBits = 0, nCells;
    uint16_t subjectID;
    float    floatVal, floatVal2;

    CanardMicrosecond transmission_deadline = getMonotonicTimestampUSec() + 1000 * 10;

    // get the subject id
    dataReturn = (int32_t *)data_getParameter(CYPHAL_ES_SUB_ID, &subjectID, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        subjectID = CYPHAL_ES_SUB_ID_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get uavcan-es-sub-id!\n");
    }

    // if there is no subject ID, dont send
    if(subjectID == 65535)
    {
        // return
        return;
    }

    // make the payload buffer
    uint8_t energySource_payload_buffer
        [reg_drone_physics_electricity_SourceTs_0_1_SERIALIZATION_BUFFER_SIZE_BYTES_];

    // make the canard transfer struct
    CanardTransfer transfer = {
        .timestamp_usec = transmission_deadline, // Zero if transmission deadline is not limited.
        .priority       = CanardPriorityNominal,
        .transfer_kind  = CanardTransferKindMessage,
        .port_id        = subjectID,            // This is the subject-ID.
        .remote_node_id = CANARD_NODE_ID_UNSET, // Messages cannot be unicast, so use UNSET.
        .transfer_id    = my_message_transfer_id,
        .payload_size   = reg_drone_physics_electricity_SourceTs_0_1_SERIALIZATION_BUFFER_SIZE_BYTES_,
        .payload        = &energySource_payload_buffer,
    };

    // make the battery status struct
    reg_drone_physics_electricity_SourceTs_0_1 energySource;

    // make the timestamp
    // leave it empty for now
    energySource.timestamp.microsecond = 0;

    // make the value

    // set the current (average?)
    dataReturn = (int32_t *)data_getParameter(I_BATT_AVG, &energySource.value.power.current.ampere, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        energySource.value.power.current.ampere = I_BATT_AVG_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get i-batt-avg!\n");
    }

    // set the voltage
    dataReturn = (int32_t *)data_getParameter(V_OUT, &energySource.value.power.voltage.volt, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        energySource.value.power.voltage.volt = V_OUT_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get v-out!\n");
    }

    // get the remaining capacity (in Ah)
    dataReturn = (int32_t *)data_getParameter(A_REM, &floatVal, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        floatVal = A_REM_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get a-rem!\n");
    }

    // get the battery voltage
    dataReturn = (int32_t *)data_getParameter(V_CELL_NOMINAL, &floatVal2, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        floatVal2 = V_CELL_NOMINAL_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get v-cell-nominal!\n");
    }

    // get the number of cells
    dataReturn = (int32_t *)data_getParameter(N_CELLS, &nCells, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        nCells = N_CELLS_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get n-cells!\n");
    }

    // calculate the energy in Joule (A_REM*V_NOM*Ncells*WH_TO_JOULE   Ah*V*Ncells*3600)
    floatVal = floatVal * floatVal2 * nCells * WH_TO_JOULE;

    // set the energy in joule
    energySource.value.energy.joule = floatVal;

    // get the full charge capacity
    dataReturn = (int32_t *)data_getParameter(A_FULL, &floatVal, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        floatVal = A_FULL_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get a-full!\n");
    }

    // calculate and set the full energy in Joule (A_FULL*V_NOM*Ncells*WH_TO_JOULE  =  Ah*V*Ncells*3600)
    energySource.value.full_energy.joule = floatVal * floatVal2 * nCells * WH_TO_JOULE;

    // convert byte to CYPHAL protocol and check for errors
    if(reg_drone_physics_electricity_SourceTs_0_1_serialize_(
           &energySource, energySource_payload_buffer, &transfer.payload_size))
    {
        cli_printfError("CYPHALCAN ERROR: energy source serialization went wrong!\n");
    }

    // set the data ready in the buffer and chop if needed
    ++my_message_transfer_id; // The transfer-ID shall be incremented after every transmission on this
                              // subject.
    int32_t result = canardTxPush(ins, &transfer);

    if(result < 0)
    {
        // An error has occurred: either an argument is invalid or we've ran out of memory.
        // It is possible to statically prove that an out-of-memory will never occur for a given application
        // if the heap is sized correctly; for background, refer to the Robson's Proof and the documentation
        // for O1Heap.
        cli_printfError("CYPHALCAN ERROR: ES Transmit error %d\n", result);
    }
}

/****************************************************************************
 * Name: BatteryStatusToTransmitBuffer
 *
 * Description:
 *   This function is called at 1 Hz rate from the main loop.
 *
 ****************************************************************************/

void BatteryStatusToTransmitBuffer(CanardInstance *ins)
{
    void *   dataReturn;
    uint8_t  statusFlagBits = 0;
    uint16_t subjectID;
    uint8_t  i, uint8Val;
    float    floatMaxValue, floatMinValue, floatValue;

    // cli_printf("BatteryStatusToTransmitBuffer!\n");

    CanardMicrosecond transmission_deadline = getMonotonicTimestampUSec() + 1000 * 10;

    // get the subject id
    dataReturn = (int32_t *)data_getParameter(CYPHAL_BS_SUB_ID, &subjectID, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        subjectID = CYPHAL_BS_SUB_ID_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get uavcan-bs-sub-id!\n");
    }

    // if there is no subject ID, dont send
    if(subjectID == 65535)
    {
        // return
        return;
    }

    // make the payload buffer
    // and remove the amount of bytes needed for more than 6 cells (float is float32 in this MCU)
    uint8_t
        batteryStatus_payload_buffer[reg_drone_service_battery_Status_0_2_SERIALIZATION_BUFFER_SIZE_BYTES_ -
            (255 - 6) * 2];

    // make the canard transfer struct
    CanardTransfer transfer = {
        .timestamp_usec = transmission_deadline, // Zero if transmission deadline is not limited.
        .priority       = CanardPriorityNominal,
        .transfer_kind  = CanardTransferKindMessage,
        .port_id        = subjectID,            // This is the subject-ID.
        .remote_node_id = CANARD_NODE_ID_UNSET, // Messages cannot be unicast, so use UNSET.
        .transfer_id    = my_message_transfer_id,
        .payload_size   = reg_drone_service_battery_Status_0_2_SERIALIZATION_BUFFER_SIZE_BYTES_,
        .payload        = &batteryStatus_payload_buffer,
    };

    // make the battery status struct
    reg_drone_service_battery_Status_0_2 batteryStatus;

    // set the hearbeat
    // set the readiness
    batteryStatus.heartbeat.readiness.value = 3;

    // set the health
    batteryStatus.heartbeat.health.value = 0;

    // get the min and max temperature
    // check if the external temperature sensor is used

    // get the sensor enable variable
    dataReturn = (int32_t *)data_getParameter(SENSOR_ENABLE, &uint8Val, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        uint8Val = SENSOR_ENABLE_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get sensor-enable!\n");
    }

    // check if it is used
    if(uint8Val)
    {
        // get the battery temperature
        dataReturn = (int32_t *)data_getParameter(C_BATT, &floatValue, NULL);

        // check for error
        if(dataReturn == NULL)
        {
            // set status flag
            statusFlagBits |= STATUS_BMS_ERROR_BIT;

            // set the value that it will be overwritten
            floatMinValue = 100;
            floatMaxValue = -40;

            // error output
            cli_printfError("CYPHALCAN ERROR: could not get c-batt!\n");
        }
        else
        {
            // set the values with the battery temperature
            floatMinValue = floatValue;
            floatMaxValue = floatValue;
        }
    }
    else
    {
        // set the value that it will be overwritten
        floatMinValue = 100;
        floatMaxValue = -40;
    }

    // loop through the cells to find the min and max
    for(i = 0; i < 3; i++)
    {
        // get the cell voltage
        dataReturn = (int32_t *)data_getParameter((parameterKind_t)(C_AFE + i), &floatValue, NULL);

        // check for error
        if(dataReturn == NULL)
        {
            // set status flag
            statusFlagBits |= STATUS_BMS_ERROR_BIT;

            // set the default value
            floatValue = C_AFE_DEFAULT;

            // error output
            cli_printfError("CYPHALCAN ERROR: could not get c-afe + %d!\n", i);
        }
        else
        {
            // check for min
            if(floatValue < floatMinValue)
            {
                // set new min
                floatMinValue = floatValue;
            }

            // check for max
            if(floatValue > floatMaxValue)
            {
                // set new max
                floatMaxValue = floatValue;
            }
        }
    }

    // set the temperaturs
    batteryStatus.temperature_min_max[0].kelvin = floatMinValue + CELSIUS_TO_KELVIN;
    batteryStatus.temperature_min_max[1].kelvin = floatMaxValue + CELSIUS_TO_KELVIN;

    // get the remaining capacity
    dataReturn = (int32_t *)data_getParameter(A_REM, &floatValue, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        floatValue = A_REM_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get a-rem!\n");
    }

    // set the amount of coulombs
    batteryStatus.available_charge.coulomb = floatValue * AMPERE_HOURS_TO_COULOMB;

    // set the error value
    batteryStatus._error.value = 0;

    // get the cell voltages and set them in the array

    // get the cell count
    dataReturn = (int32_t *)data_getParameter(N_CELLS, &uint8Val, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        uint8Val = N_CELLS_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get n-cells!\n");
    }

    // loop through the cells to set them
    for(i = 0; i < uint8Val; i++)
    {
        // get the cell voltage
        dataReturn = (int32_t *)data_getParameter((parameterKind_t)(V_CELL1 + i), &floatValue, NULL);

        // check for error
        if(dataReturn == NULL)
        {
            // set status flag
            statusFlagBits |= STATUS_BMS_ERROR_BIT;

            // set the default value
            floatValue = V_CELL1_DEFAULT;

            // error output
            cli_printfError("CYPHALCAN ERROR: could not get v-cell%d!\n", i);
        }

        // set the cell volatge in the array
        batteryStatus.cell_voltages.elements[i] = floatValue;
    }

    // set the cell count
    batteryStatus.cell_voltages.count = (size_t)uint8Val;

    // convert byte to CYPHAL protocol
    if(reg_drone_service_battery_Status_0_2_serialize_(
           &batteryStatus, batteryStatus_payload_buffer, &transfer.payload_size))
    {
        cli_printfError("CYPHALCAN ERROR: battery status serialization went wrong!\n");
    }

    // set the data ready in the buffer and chop if needed
    ++my_message_transfer_id; // The transfer-ID shall be incremented after every transmission on this
                              // subject.
    int32_t result = canardTxPush(ins, &transfer);

    if(result < 0)
    {
        // An error has occurred: either an argument is invalid or we've ran out of memory.
        // It is possible to statically prove that an out-of-memory will never occur for a given application
        // if the heap is sized correctly; for background, refer to the Robson's Proof and the documentation
        // for O1Heap.
        cli_printfError("CYPHALCAN ERROR: BS Transmit error %d\n", result);
    }
}

/****************************************************************************
 * Name: BatteryParametersToTransmitBuffer
 *
 * Description:
 *   This function is called at 1 Hz rate from the main loop.
 *
 ****************************************************************************/

void BatteryParametersToTransmitBuffer(CanardInstance *ins)
{
    void *   dataReturn;
    uint8_t  statusFlagBits = 0;
    uint16_t subjectID, chargeFullCurrentmA;
    uint8_t  uint8Val;
    float    floatValue;

    CanardMicrosecond transmission_deadline = getMonotonicTimestampUSec() + 1000 * 10;

    // get the subject id
    dataReturn = (int32_t *)data_getParameter(CYPHAL_BP_SUB_ID, &subjectID, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        subjectID = CYPHAL_BP_SUB_ID_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get uavcan-bp-sub-id!\n");
    }

    // if there is no subject ID, dont send
    if(subjectID == 65535)
    {
        // return
        return;
    }

    // make the payload buffer
    uint8_t batteryParameters_payload_buffer
        [reg_drone_service_battery_Parameters_0_3_SERIALIZATION_BUFFER_SIZE_BYTES_];

    // make the canard transfer struct
    CanardTransfer transfer = {
        .timestamp_usec = transmission_deadline, // Zero if transmission deadline is not limited.
        .priority       = CanardPriorityNominal,
        .transfer_kind  = CanardTransferKindMessage,
        .port_id        = subjectID,            // This is the subject-ID.
        .remote_node_id = CANARD_NODE_ID_UNSET, // Messages cannot be unicast, so use UNSET.
        .transfer_id    = my_message_transfer_id,
        .payload_size   = reg_drone_service_battery_Parameters_0_3_SERIALIZATION_BUFFER_SIZE_BYTES_,
        .payload        = &batteryParameters_payload_buffer,
    };

    // make the battery parameters struct
    reg_drone_service_battery_Parameters_0_3 batteryParameters;

    // set the unique id
    dataReturn = (int32_t *)data_getParameter(MODEL_ID, &batteryParameters.unique_id, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        batteryParameters.unique_id = MODEL_ID_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get model-id!\n");
    }

    // set the mass
    dataReturn = (int32_t *)data_getParameter(M_MASS, &batteryParameters.mass.kilogram, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        batteryParameters.mass.kilogram = M_MASS_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get m-mass!\n");
    }

    // set the design capacity
    // get the design capacity
    dataReturn = (int32_t *)data_getParameter(A_FACTORY, &floatValue, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        floatValue = A_FACTORY_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get a-factory!\n");
    }

    // set the design capacity
    batteryParameters.design_capacity.coulomb = floatValue * AMPERE_HOURS_TO_COULOMB;

    // set the design_cell_voltage_min_max
    // set the min voltage
    dataReturn =
        (int32_t *)data_getParameter(V_CELL_UV, &batteryParameters.design_cell_voltage_min_max[0].volt, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        batteryParameters.design_cell_voltage_min_max[0].volt = V_CELL_UV_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get v-cell-uv!\n");
    }

    // set the max voltage
    dataReturn =
        (int32_t *)data_getParameter(V_CELL_OV, &batteryParameters.design_cell_voltage_min_max[1].volt, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        batteryParameters.design_cell_voltage_min_max[1].volt = V_CELL_OV_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get v-cell-ov!\n");
    }

    // set the discharge_current
    dataReturn =
        (int32_t *)data_getParameter(I_OUT_NOMINAL, &batteryParameters.discharge_current.ampere, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        batteryParameters.discharge_current.ampere = I_OUT_NOMINAL_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get i-out-nominal!\n");
    }

    // set the discharge_current_burst
    dataReturn =
        (int32_t *)data_getParameter(I_OUT_MAX, &batteryParameters.discharge_current_burst.ampere, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        batteryParameters.discharge_current_burst.ampere = I_OUT_MAX_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get i-out-max!\n");
    }

    // set the charge_current
    dataReturn =
        (int32_t *)data_getParameter(I_CHARGE_NOMINAL, &batteryParameters.charge_current.ampere, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        batteryParameters.charge_current.ampere = I_CHARGE_NOMINAL_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get i-charge-nominal!\n");
    }

    // set the charge_current_fast
    dataReturn =
        (int32_t *)data_getParameter(I_CHARGE_MAX, &batteryParameters.charge_current_fast.ampere, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        batteryParameters.charge_current_fast.ampere = I_CHARGE_MAX_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get i-charge-max!\n");
    }

    // set the charge_termination_threshold
    dataReturn = (int32_t *)data_getParameter(I_CHARGE_FULL, &chargeFullCurrentmA, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        chargeFullCurrentmA = I_CHARGE_FULL_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get i-charge-full!\n");
    }

    batteryParameters.charge_termination_threshold.ampere = (float)chargeFullCurrentmA / 1000.0;

    // set the charge_voltage
    dataReturn = (int32_t *)data_getParameter(V_CELL_OV, &batteryParameters.charge_voltage.volt, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        batteryParameters.charge_voltage.volt = V_CELL_OV_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get v-cell-ov!\n");
    }

    // set the cycle_count
    dataReturn = (int32_t *)data_getParameter(N_CHARGES, &batteryParameters.cycle_count, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        batteryParameters.cycle_count = N_CHARGES_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get n-charges!\n");
    }

    // // set the series_cell_count
    // dataReturn = (int32_t*)data_getParameter(N_CELLS, &batteryParameters.series_cell_count, NULL);

    //    // check for error
    //    if(dataReturn == NULL)
    //    {
    //     // set status flag
    //     statusFlagBits |= STATUS_BMS_ERROR_BIT;

    //     // set the default value
    //     batteryParameters.series_cell_count = N_CELLS_DEFAULT;
    //    }

    // set the state_of_health_pct
    dataReturn = (int32_t *)data_getParameter(S_HEALTH, &batteryParameters.state_of_health_pct, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        batteryParameters.state_of_health_pct = S_HEALTH_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get s-health!\n");
    }

    // check for limit
    if(batteryParameters.state_of_health_pct > 100)
    {
        // limit the value
        batteryParameters.state_of_health_pct = 100;
    }

    // get the battery type
    dataReturn = (int32_t *)data_getParameter(BATTERY_TYPE, &uint8Val, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        uint8Val = BATTERY_TYPE_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get battery-type!\n");
    }

    // check if the technology is either LiPo or LiFePO4
    switch(uint8Val)
    {
        case 0:
            // set the Technology
            batteryParameters.technology.value = LIPO_TECHNOLOGY; 
        break;
        case 1:
            batteryParameters.technology.value = LIPO_TECHNOLOGY + 1; // LFP 
        break;
        case 2:
            batteryParameters.technology.value = LIPO_TECHNOLOGY + 1; // LFP (just state LFP, most close)
        break;
        case 3:
            batteryParameters.technology.value = 102; // NMC LIPO
        break;
        case 4:
            batteryParameters.technology.value = 0; // unspecified. Sodium-ion is not yet in the list at this time.
        break;
        default:
            // set the Technology
            batteryParameters.technology.value = 0; // unspecified. 
        break;
    }

    // get the nominal voltage
    dataReturn = (int32_t *)data_getParameter(V_CELL_NOMINAL, &floatValue, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        floatValue = V_CELL_NOMINAL_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get v-cell-nominal!\n");
    }

    // get the number of cells
    dataReturn = (int32_t *)data_getParameter(N_CELLS, &uint8Val, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        uint8Val = N_CELLS_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get n-cells!\n");
    }

    // multiply the nominal cell voltage with the amount of cells to get the battery voltage
    batteryParameters.nominal_voltage.volt = floatValue * uint8Val;

    // convert byte to CYPHAL protocol
    if(reg_drone_service_battery_Parameters_0_3_serialize_(
           &batteryParameters, batteryParameters_payload_buffer, &transfer.payload_size))
    {
        cli_printfError("CYPHALCAN ERROR: battery parameters serialization went wrong!\n");
    }

    // set the data ready in the buffer and chop if needed
    ++my_message_transfer_id; // The transfer-ID shall be incremented after every transmission on this
                              // subject.
    int32_t result = canardTxPush(ins, &transfer);

    if(result < 0)
    {
        // An error has occurred: either an argument is invalid or we've ran out of memory.
        // It is possible to statically prove that an out-of-memory will never occur for a given application
        // if the heap is sized correctly; for background, refer to the Robson's Proof and the documentation
        // for O1Heap.
        cli_printfError("CYPHALCAN ERROR: BP Transmit error %d\n", result);
    }
}

/*!
 * @brief   this function will flush the can TX
 *
 * @return  If successful, the function will return zero (OK). Otherwise, an error number will be returned to
 * indicate the error:
 *
 */
int cyphalcan_flushtx(void)
{
    eventfd_t value = 1ULL;

    return file_write(gEventfp, &value, sizeof(value));
}
/****************************************************************************
 * Name: BatteryInfoToTransmitBuffer
 *
 * Description:
 *   This function is called at 0.2 ~ 1 Hz rate from the main loop to send
 *   the legacy battery info message.
 *
 ****************************************************************************/

void BatteryInfoToTransmitBuffer(CanardInstance *ins)
{
    void *   dataReturn;
    uint8_t  uint8Val, statusFlagBits = 0, nCells, i;
    uint16_t subjectID, modelNameSize = 0;
    float    floatVal, floatVal2, floatMinValue, floatMaxValue;
    uint64_t modelId;

    CanardMicrosecond transmission_deadline = getMonotonicTimestampUSec() + 1000 * 10;

    // get the subject id
    dataReturn = (int32_t *)data_getParameter(CYPHAL_LEGACY_BI_SUB_ID, &subjectID, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        subjectID = CYPHAL_LEGACY_BI_SUB_ID_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get uavcan-legacy-bi-sub-id!\n");
    }

    // if there is no subject ID, dont send
    if(subjectID == 65535)
    {
        // cli_printfError("CYPHALCAN ERROR: uavcan-es-sub-id is 65535, not outputing message!\n");

        // return
        return;
    }

    // make the payload buffer
    uint8_t
        batteryInfo_payload_buffer[legacy_equipment_power_BatteryInfo_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_];

    // make the canard transfer struct
    CanardTransfer transfer = {
        .timestamp_usec = transmission_deadline, // Zero if transmission deadline is not limited.
        .priority       = CanardPriorityNominal,
        .transfer_kind  = CanardTransferKindMessage,
        .port_id        = subjectID,            // This is the subject-ID.
        .remote_node_id = CANARD_NODE_ID_UNSET, // Messages cannot be unicast, so use UNSET.
        .transfer_id    = my_message_transfer_id,
        .payload_size   = legacy_equipment_power_BatteryInfo_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_,
        .payload        = &batteryInfo_payload_buffer,
    };

    // make the battery status struct
    legacy_equipment_power_BatteryInfo_1_0 batteryInfo;

    // set the temperature
    dataReturn = (int32_t *)data_getParameter(C_BATT, &batteryInfo.temperature, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        batteryInfo.temperature = C_BATT_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get c-batt!\n");
    }

    // set the voltage
    dataReturn = (int32_t *)data_getParameter(V_OUT, &batteryInfo.voltage, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        batteryInfo.voltage = V_OUT_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get v-out!\n");
    }

    // set the current (average?)
    dataReturn = (int32_t *)data_getParameter(I_BATT_AVG, &batteryInfo.current, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        batteryInfo.current = I_BATT_AVG_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get i-batt-avg!\n");
    }

    // set the average power 10 sec
    dataReturn = (int32_t *)data_getParameter(P_AVG, &batteryInfo.average_power_10sec, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        batteryInfo.average_power_10sec = P_AVG_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get p-avg!\n");
    }

    // get the remaining capacity (in Ah)
    dataReturn = (int32_t *)data_getParameter(A_REM, &floatVal, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        floatVal = A_REM_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get a-rem!\n");
    }

    // get the cell nominal voltage
    dataReturn = (int32_t *)data_getParameter(V_CELL_NOMINAL, &floatVal2, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        floatVal2 = V_CELL_NOMINAL_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get v-batt!\n");
    }

    // get the number of cells
    dataReturn = (int32_t *)data_getParameter(N_CELLS, &nCells, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        nCells = N_CELLS_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get n-cells!\n");
    }

    // calculate the energy in Wh (Ah*Vcell*cells)
    floatVal = floatVal * floatVal2 * nCells;

    // set the energy in wh
    batteryInfo.remaining_capacity_wh = floatVal;

    // get the full charge capacity
    dataReturn = (int32_t *)data_getParameter(A_FULL, &floatVal, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        floatVal = A_FULL_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get a-full!\n");
    }

    // calculate and set the full charge capacity in wh (A_FULL * V_CELL_NOMINAL * nCells)
    batteryInfo.full_charge_capacity_wh = floatVal * floatVal2 * nCells;

    // set the hours to full charge
    dataReturn = (int32_t *)data_getParameter(T_FULL, &batteryInfo.hours_to_full_charge, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        batteryInfo.hours_to_full_charge = T_FULL_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get t-full!\n");
    }

    // set the state of health
    dataReturn = (int32_t *)data_getParameter(S_HEALTH, &batteryInfo.state_of_health_pct, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        batteryInfo.state_of_health_pct = S_HEALTH_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get s-health!\n");
    }

    // set the state of charge
    dataReturn = (int32_t *)data_getParameter(S_CHARGE, &batteryInfo.state_of_charge_pct, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        batteryInfo.state_of_charge_pct = S_CHARGE_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get s-charge!\n");
    }

    // set the state of charge stdev value
    batteryInfo.state_of_charge_pct_stdev = 0;

    // set the battery id
    dataReturn = (int32_t *)data_getParameter(BATT_ID, &batteryInfo.battery_id, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        batteryInfo.battery_id = BATT_ID_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get batt-id!\n");
    }

    // get the model id
    dataReturn = (int32_t *)data_getParameter(MODEL_ID, &modelId, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        modelId = MODEL_ID_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get model-id!\n");
    }

    // set the model_id
    batteryInfo.model_instance_id = (uint32_t)modelId;

    // set the model name and the size
    dataReturn =
        (int32_t *)data_getParameter(MODEL_NAME, &batteryInfo.model_name.elements[0], &modelNameSize);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        strcpy((char *)batteryInfo.model_name.elements, (char *)MODEL_NAME_DEFAULT);

        // set the size to 0
        modelNameSize = 0;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get model-name!\n");
    }

    // set the model name size
    batteryInfo.model_name.count = (size_t)modelNameSize;

    // reset the status flags but set the error if there is one
    batteryInfo.status_flags = ((uint16_t)statusFlagBits << 3) &
        legacy_equipment_power_BatteryInfo_1_0_STATUS_FLAG_BMS_ERROR; // do last

    // get the battery status flags
    dataReturn = (int32_t *)data_getParameter(S_FLAGS, &statusFlagBits, NULL);

    // check if it is the unknown value
    if(statusFlagBits == S_FLAGS_UKNOWN)
    {
        // reset it to 0
        statusFlagBits = 0;
    }

    // check for error
    if(dataReturn == NULL)
    {
        // set the default value
        statusFlagBits = 0;

        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get model-id!\n");
    }

    // set the correct bits for everything except in use, charing, charged, temp hot, temp cold
    batteryInfo.status_flags |= (((uint16_t)statusFlagBits & 0x00FC) << 3);

    // get the sleep current th
    dataReturn = (int32_t *)data_getParameter(I_SLEEP_OC, &uint8Val, NULL);

    // check for error
    if(dataReturn == NULL)
    {
        // set status flag
        statusFlagBits |= STATUS_BMS_ERROR_BIT;

        // set the default value
        uint8Val = I_SLEEP_OC_DEFAULT;

        // error output
        cli_printfError("CYPHALCAN ERROR: could not get i-sleep-oc!\n");
    }

    // check if in use
    if((batteryInfo.current > ((float)uint8Val / 1000.0)) ||
        (batteryInfo.current < ((float)uint8Val / -1000.0)))
    {
        // set the battery in use on
        batteryInfo.status_flags |= legacy_equipment_power_BatteryInfo_1_0_STATUS_FLAG_IN_USE;
    }

    // check if charging
    if(data_getMainState() == CHARGE)
    {
        // set the charging bit high
        batteryInfo.status_flags |= legacy_equipment_power_BatteryInfo_1_0_STATUS_FLAG_CHARGING;

        // check if done charging
        // check current charge state
        if(data_getChargeState() == CHARGE_COMPLETE)
        {
            // set the charging done bit
            batteryInfo.status_flags |= legacy_equipment_power_BatteryInfo_1_0_STATUS_FLAG_CHARGED;
        }
    }

    // check for temperature error
    if(statusFlagBits & (1 << STATUS_TEMP_ERROR_BIT))
    {
        // get the highest and lowest temperature

        // get the sensor enable variable
        dataReturn = (int32_t *)data_getParameter(SENSOR_ENABLE, &uint8Val, NULL);

        // check for error
        if(dataReturn == NULL)
        {
            // set status flag
            statusFlagBits |= STATUS_BMS_ERROR_BIT;

            // set the default value
            uint8Val = SENSOR_ENABLE_DEFAULT;

            // error output
            cli_printfError("CYPHALCAN ERROR: could not get sensor-enable!\n");
        }

        // check if it is used
        if(uint8Val)
        {
            // get the battery temperature
            dataReturn = (int32_t *)data_getParameter(C_BATT, &floatVal, NULL);

            // check for error
            if(dataReturn == NULL)
            {
                // set status flag
                statusFlagBits |= STATUS_BMS_ERROR_BIT;

                // set the value that it will be overwritten
                floatMinValue = 100;
                floatMaxValue = -40;

                // error output
                cli_printfError("CYPHALCAN ERROR: could not get c-batt!\n");
            }
            else
            {
                // set the values with the battery temperature
                floatMinValue = floatVal;
                floatMaxValue = floatVal;
            }
        }
        else
        {
            // set the value that it will be overwritten
            floatMinValue = 100;
            floatMaxValue = -40;
        }

        // loop through the cells to find the min and max
        for(i = 0; i < 3; i++)
        {
            // get the cell voltage
            dataReturn = (int32_t *)data_getParameter((parameterKind_t)(C_AFE + i), &floatVal, NULL);

            // check for error
            if(dataReturn == NULL)
            {
                // set status flag
                statusFlagBits |= STATUS_BMS_ERROR_BIT;

                // set the default value
                floatVal = V_CELL1_DEFAULT;

                // error output
                cli_printfError("CYPHALCAN ERROR: could not get c-afe + %d!\n", i);
            }
            else
            {
                // check for min
                if(floatVal < floatMinValue)
                {
                    // set new min
                    floatMinValue = floatVal;
                }

                // check for max
                if(floatVal > floatMaxValue)
                {
                    // set new max
                    floatMaxValue = floatVal;
                }
            }
        }

        // get battery over and under temperature values
        dataReturn = (int32_t *)data_getParameter(C_CELL_OT, &floatVal, NULL);

        // check for error
        if(dataReturn == NULL)
        {
            // set status flag
            statusFlagBits |= STATUS_BMS_ERROR_BIT;

            // set the default value
            floatVal = C_CELL_OT_DEFAULT;

            // error output
            cli_printfError("CYPHALCAN ERROR: could not get c-cell-ot!\n");
        }

        // get the cell under temperature
        dataReturn = (int32_t *)data_getParameter(C_CELL_UT, &floatVal2, NULL);

        // check for error
        if(dataReturn == NULL)
        {
            // set status flag
            statusFlagBits |= STATUS_BMS_ERROR_BIT;

            // set the default value
            floatVal2 = C_CELL_UT_DEFAULT;

            // error output
            cli_printfError("CYPHALCAN ERROR: could not get c-cell-ut!\n");
        }

        // check for too hot or too cold error
        if((floatVal - floatMaxValue) < (floatMinValue - floatVal2))
        {
            // set the battery temp hot error on
            batteryInfo.status_flags |= legacy_equipment_power_BatteryInfo_1_0_STATUS_FLAG_TEMP_HOT;
        }
        else
        {
            // set the battery temp cold error on
            batteryInfo.status_flags |= legacy_equipment_power_BatteryInfo_1_0_STATUS_FLAG_TEMP_COLD;
        }
    }

    // convert byte to CYPHAL protocol and check for errors
    if(legacy_equipment_power_BatteryInfo_1_0_serialize_(
           &batteryInfo, batteryInfo_payload_buffer, &transfer.payload_size))
    {
        cli_printfError("CYPHALCAN ERROR: battery info serialization went wrong!\n");
    }

    // set the data ready in the buffer and chop if needed
    ++my_message_transfer_id; // The transfer-ID shall be incremented after every transmission on this
                              // subject.
    int32_t result = canardTxPush(ins, &transfer);

    if(result < 0)
    {
        // An error has occurred: either an argument is invalid or we've ran out of memory.
        // It is possible to statically prove that an out-of-memory will never occur for a given application
        // if the heap is sized correctly; for background, refer to the Robson's Proof and the documentation
        // for O1Heap.
        cli_printfError("CYPHALCAN ERROR: Transmit error %d\n", result);
    }
}

// static void processReceivedTransfer(CanardTransfer *receive)
// {
//  cli_printf("Received transfer remote_node_id %d transfer_id: %d payload size: %d\n",
//         receive->remote_node_id, receive->transfer_id, receive->payload_size);

// }

/****************************************************************************
 * Name: processTxRxOnce
 *
 * Description:
 *   Transmits all frames from the TX queue, receives up to one frame.
 *
 ****************************************************************************/

bool processTxRxOnce(CanardInstance *ins, CanardSocketInstance *sock_ins, int timeout_msec)
{
    int32_t result;
    bool publish = false;

    /* Transmitting */
    for(const CanardFrame *txf = NULL; (txf = canardTxPeek(ins)) != NULL;)
    { // Look at the top of the TX queue.
        if(txf->timestamp_usec > getMonotonicTimestampUSec())
        { // Check if the frame has timed out.
            if(socketcanCyphalTransmit(sock_ins, txf) == 0)
            {          // Send the frame. Redundant interfaces may be used here.
                break; // If the driver is busy, break and retry later.
            }
        }

        canardTxPop(ins);                          // Remove the frame from the queue after it's transmitted.
        ins->memory_free(ins, (CanardFrame *)txf); // Deallocate the dynamic memory afterwards.
    }

    // wait for either can messages or the BMS application
    if(poll(pfds, 2, -1) > 0)
    {
        // if it is CAN communication
        if(pfds[0].revents & POLLIN)
        {

            /* Receiving */
            CanardFrame received_frame;

            socketcanCyphalReceive(sock_ins, &received_frame);

            CanardTransfer receive;
            result = canardRxAccept(ins,
                &received_frame, // The CAN frame received from the bus.
                0,               // If the transport is not redundant, use 0.
                &receive);

            if(result < 0)
            {
                // An error has occurred: either an argument is invalid or we've ran out of memory.
                // It is possible to statically prove that an out-of-memory will never occur for a given
                // application if the heap is sized correctly; for background, refer to the Robson's Proof and
                // the documentation for O1Heap. Reception of an invalid frame is NOT an error.
                cli_printfError("CYPHALCAN ERROR: Receive error %d\n", result);
            }
            else if(result == 1)
            {
                // A transfer has been received, process it. !!!!

                if(receive.port_id == PNPGetPortID(ins))
                {
                    PNPProcess(ins, &receive);
                }
                else
                {
                    cyphal_register_interface_process(ins, &receive);
                }

                ins->memory_free(ins, (void *)receive.payload); // Deallocate the dynamic memory afterwards.

                /* Transmitting */
                for(const CanardFrame *txf = NULL; (txf = canardTxPeek(ins)) != NULL;)
                { // Look at the top of the TX queue.
                    if(txf->timestamp_usec > getMonotonicTimestampUSec())
                    { // Check if the frame has timed out.
                        if(socketcanCyphalTransmit(sock_ins, txf) == 0)
                        {          // Send the frame. Redundant interfaces may be used here.
                            break; // If the driver is busy, break and retry later.
                        }
                    }

                    canardTxPop(ins); // Remove the frame from the queue after it's transmitted.
                    ins->memory_free(ins, (CanardFrame *)txf); // Deallocate the dynamic memory afterwards.
                }
            }
            else
            {
                // cli_printf("RX canard %d\r\n", result);
                // Nothing to do.
                // The received frame is either invalid or it's a non-last frame of a multi-frame transfer.
                // Reception of an invalid frame is NOT reported as an error because it is not an error.
            }
        }

        // the event is triggered by the BMS application to send BMS status
        if(pfds[1].revents & POLLIN)
        {
            eventfd_t value;
            file_read(gEventfp, &value, sizeof(value));
            publish = true;
        }
    }

    // return if to publish the BMS data on CyphalCAN
    return publish;
}
