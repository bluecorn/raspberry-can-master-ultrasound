/// This software is distributed under the terms of the MIT License.
/// Copyright (c) 2020 Hugo A. Garcia
/// Author Hugo A. Garcia <hugo.a.garcia@gmail.com>
/// Based on code by:
///     Pavel Kirienko <pavel.kirienko@zubax.com>
///     joan2937 <joan@abyz.me.uk>

#include "socketcan.h"
#include "canard.h"
#include "canard_dsdl.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

/* Message Subject ID's 
 * where ID = [0, 24575]
 * for mandatory Heartbeat Message;
 * for Ultrasound Message,
 * ref. Unregulated identifiers (both fixed and non-fixed).
 *      SpecificationRevision v1.0-alpha, sec. 5.1.1
 */
static const uint16_t HeartbeatSubjectID = 32085;
static const uint16_t UltrasoundMessageSubjectID = 1610;

// Memory management.
static void *canardAllocate(CanardInstance *const ins, const size_t amount)
{
    (void)ins;
    return malloc(amount);
}

static void canardFree(CanardInstance *const ins, void *const pointer)
{
    (void)ins;
    free(pointer);
}

/* Node heartbeat
 * ref. SpecificationRevision v1.0-alpha, sec. 5.3.2
 */
static void publishHeartbeat(CanardInstance *const canard, const uint32_t uptime)
{
    static CanardTransferID transfer_id;
    const uint8_t payload[7] = {
        (uint8_t)(uptime >> 0U),
        (uint8_t)(uptime >> 8U),
        (uint8_t)(uptime >> 16U),
        (uint8_t)(uptime >> 24U),
        0,
        0,
        0,
    };
    const CanardTransfer transfer = {
        .priority = CanardPriorityNominal,
        .transfer_kind = CanardTransferKindMessage,
        .port_id = HeartbeatSubjectID,
        .remote_node_id = CANARD_NODE_ID_UNSET,
        .transfer_id = transfer_id,
        .payload_size = sizeof(payload),
        .payload = &payload[0],
    };
    ++transfer_id;
    (void)canardTxPush(canard, &transfer);
}

// Making sure it lives
static void handleHeartbeat()
{
    printf("%s \n", "Thump thump");
}

/*
 * MAIN 
 */
int main(const int argc, const char *const argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage:   %s <iface-name> <node-id>\n", argv[0]);
        fprintf(stderr, "Example: %s vcan0 42\n", argv[0]);
        return 1;
    }

    // Initialize the node with a static node-ID as specified in the command-line arguments.
    CanardInstance canard = canardInit(&canardAllocate, &canardFree);
    canard.mtu_bytes = CANARD_MTU_CAN_CLASSIC; // Do not use CAN FD to enhance compatibility.
    canard.node_id = (CanardNodeID)atoi(argv[2]);

    // Initialize a SocketCAN socket. Do not use CAN FD to enhance compatibility.
    const SocketCANFD sock = socketcanOpen(argv[1], false);
    if (sock < 0)
    {
        fprintf(stderr, "Could not initialize the SocketCAN interface: errno %d %s\n", -sock, strerror(-sock));
        return 1;
    }

    /*
    Subscriptions to meessages.
    */

    // Heatbeat subscription
    CanardRxSubscription heartbeat_subscription;
    (void)canardRxSubscribe(&canard, // Subscribe to messages uavcan.node.Heartbeat.
                            CanardTransferKindMessage,
                            HeartbeatSubjectID, // The fixed Subject-ID of the Heartbeat message type (see DSDL definition).
                            7,                  // The maximum payload size (max DSDL object size) from the DSDL definition.
                            CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC,
                            &heartbeat_subscription);
    
    // Ultrasound subscription
    CanardRxSubscription ultrasound_subscription;
    (void)canardRxSubscribe(&canard, // Subscribe to messages uavcan.node.Heartbeat.
                            CanardTransferKindMessage,
                            UltrasoundMessageSubjectID,
                            7,                  
                            CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC,
                            &ultrasound_subscription);

    // The main loop: publish messages and process service requests.
    const time_t boot_ts = time(NULL);
    time_t next_1hz_at = boot_ts;
    while (true)
    {
        if (next_1hz_at < time(NULL))
        {
            next_1hz_at++;
            publishHeartbeat(&canard, time(NULL) - boot_ts);
        }

        // Transmit pending frames.
        const CanardFrame *txf = canardTxPeek(&canard);
        while (txf != NULL)
        {
            printf("Transfer\n");
            (void)socketcanPush(sock, txf, 0); // Error handling not implemented
            canardTxPop(&canard);
            free((void *)txf);
            txf = canardTxPeek(&canard);
        }

        // Process received frames, if any.
        CanardFrame rxf;
        uint8_t buffer[64];
        while (socketcanPop(sock, &rxf, sizeof(buffer), buffer, 1000) > 0) // Error handling not implemented
        {
            
            printf("Recieve\n");

            CanardTransfer transfer;
            if (canardRxAccept(&canard, &rxf, 0, &transfer))
            {
                // Can be refactored with a switch on transfer.port_id //
                
                if ((transfer.transfer_kind == CanardTransferKindMessage) &&
                    (transfer.port_id == HeartbeatSubjectID))
                {
                    handleHeartbeat();
                }
                if ((transfer.transfer_kind == CanardTransferKindMessage) &&
                    (transfer.port_id == UltrasoundMessageSubjectID))
                {
                    // Deserialize distance from ultrasound message
                    CanardDSDLFloat32 distance = canardDSDLGetF32(transfer.payload, transfer.payload_size, 0);
                    
                    //Debugging
                    printf("%f\n", distance);
                }

                free((void *)transfer.payload);
            }
        }
    }
}
