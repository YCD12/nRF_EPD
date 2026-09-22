/**
 * BLE Image Transfer Module with CRC16 Checksum and Resume Support
 * 
 * Features:
 * 1. CRC16-CCITT verification for data integrity
 * 2. Batch confirmation mode - send N blocks then verify
 * 3. Resume capability - reconnect and continue from last position
 * 4. Multi-layer support for three-color displays
 * 5. Transfer speed display
 * 6. Disconnect detection and recovery
 * 7. Configurable logging levels
 */

const BleTransfer = {
    // Configuration constants
    MAX_RETRIES: 3,          // Maximum retry rounds
    BATCH_SIZE: 20,          // Blocks per batch before status check (tuned for mobile stability)
    BATCH_DELAY_MS: 150,     // Delay after batch for MCU processing (tuned for mobile stability)

    // Logging level: 0=none, 1=errors, 2=info, 3=debug
    logLevel: 2,

    // State variables
    sessionId: 0,
    currentLayer: 0x0F,      // Current layer: 0x0F=BW, 0x00=color
    pendingStatus: null,
    statusResolver: null,
    statusRequestId: 0,
    block0Sent: false,       // Track if block 0 has been sent for current layer
    restartRequested: false, // MCU 0x03: sequential stream must restart from 0

    // Statistics for speed calculation
    transferStats: {
        startTime: 0,
        bytesSent: 0,
        blocksSent: 0
    },

    /**
     * Logging helper with level control
     * @param {number} level - Log level (1=error, 2=info, 3=debug)
     * @param {string} message - Message to log
     * @param {any} data - Optional data to log
     */
    log(level, message, data = null) {
        if (level > this.logLevel) return;

        const prefix = '[BleTransfer]';
        if (level === 1) {
            if (data) console.error(prefix, message, data);
            else console.error(prefix, message);
        } else if (level === 2) {
            if (data) console.log(prefix, message, data);
            else console.log(prefix, message);
        } else {
            if (data) console.debug(prefix, message, data);
            else console.debug(prefix, message);
        }
    },

    /**
     * Check if BLE is still connected
     * @returns {boolean} True if connected
     */
    isConnected() {
        return typeof epdCharacteristic !== 'undefined' &&
            epdCharacteristic !== null &&
            typeof bleDevice !== 'undefined' &&
            bleDevice !== null &&
            bleDevice.gatt &&
            bleDevice.gatt.connected;
    },

    /**
     * CRC16-CCITT calculation
     * @param {Uint8Array} data - Data to checksum
     * @returns {number} - 16-bit CRC value
     */
    crc16(data) {
        let crc = 0xFFFF;
        for (let i = 0; i < data.length; i++) {
            crc ^= data[i];
            for (let j = 0; j < 8; j++) {
                crc = (crc & 1) ? (crc >>> 1) ^ 0x8408 : crc >>> 1;
            }
        }
        return crc & 0xFFFF;
    },

    /**
     * Calculate transfer speed
     * @returns {Object} Speed info { bytesPerSecond, kbps, elapsed }
     */
    getTransferSpeed() {
        const elapsed = (Date.now() - this.transferStats.startTime) / 1000;
        if (elapsed <= 0) return { bytesPerSecond: 0, kbps: 0, elapsed: 0 };

        const bytesPerSecond = this.transferStats.bytesSent / elapsed;
        return {
            bytesPerSecond: bytesPerSecond,
            kbps: (bytesPerSecond * 8 / 1000).toFixed(1),
            elapsed: elapsed.toFixed(1)
        };
    },

    /**
     * Format speed for display
     * @returns {string} Formatted speed string (e.g., "12.5 KB/s")
     */
    getSpeedString() {
        const speed = this.getTransferSpeed();
        if (speed.bytesPerSecond < 1024) {
            return `${speed.bytesPerSecond.toFixed(0)} B/s`;
        } else {
            return `${(speed.bytesPerSecond / 1024).toFixed(1)} KB/s`;
        }
    },

    /**
     * Handle MCU notification (call from main.js handleNotify)
     * @param {DataView|Uint8Array} value - Notification data
     */
    handleNotification(value) {
        let data;
        if (value instanceof DataView) {
            data = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
        } else if (value instanceof Uint8Array) {
            data = value;
        } else {
            data = new Uint8Array(value);
        }

        if (data[0] === 0xA0) {
            // Block ACK/NACK: [0xA0, block_id_L, block_id_H, status]
            // 0x00 OK, 0x01 CRC, 0x02 invalid, 0x03 restart from 0
            const blockId = data[1] | (data[2] << 8);
            const status = data[3];
            if (status === 0x03) {
                this.restartRequested = true;
                this.log(2, `Block ${blockId} requested restart from 0`);
            } else {
                this.log(3, `Block ${blockId} ACK: ${status === 0 ? 'OK' : 'FAIL ' + status}`);
            }
        } else if (data[0] === 0xA1) {
            // [0xA1, total_L, total_H, received_L, received_H, session, active]
            this.pendingStatus = {
                total: data[1] | (data[2] << 8),
                received: data[3] | (data[4] << 8),
                sessionId: data[5],
                active: data[6] === 1
            };
            this.log(3, 'Status:', this.pendingStatus);

            if (this.statusResolver) {
                const resolver = this.statusResolver;
                this.statusResolver = null;
                resolver(this.pendingStatus);
            }
        }
    },

    /**
     * Query MCU transfer status with timeout
     * @param {number} timeout - Timeout in milliseconds
     * @returns {Promise<Object|null>} - Transfer status or null
     */
    async queryStatus(timeout = 2000) {
        // Check connection first
        if (!this.isConnected()) {
            this.log(1, 'Cannot query status: BLE disconnected');
            throw new Error('BLE disconnected');
        }

        this.pendingStatus = null;
        this.statusRequestId++;
        const requestId = this.statusRequestId;

        return new Promise(async (resolve) => {
            const timer = setTimeout(() => {
                if (this.statusRequestId === requestId) {
                    this.statusResolver = null;
                    resolve(this.pendingStatus);
                }
            }, timeout);

            this.statusResolver = (status) => {
                clearTimeout(timer);
                resolve(status);
            };

            try {
                await write(EpdCmd.QUERY_STATUS);
            } catch (e) {
                clearTimeout(timer);
                this.statusResolver = null;
                this.log(1, 'Query status failed:', e);
                resolve(null);
            }
        });
    },

    /**
     * Reset MCU transfer state
     * @param {number} newSessionId - Optional new session ID
     */
    async resetTransfer(newSessionId) {
        // Check connection first
        if (!this.isConnected()) {
            this.log(1, 'Cannot reset transfer: BLE disconnected');
            throw new Error('BLE disconnected');
        }

        this.sessionId = newSessionId !== undefined ? newSessionId : (Date.now() & 0xFF);
        this.block0Sent = false;
        this.restartRequested = false;

        // Reset statistics
        this.transferStats = {
            startTime: Date.now(),
            bytesSent: 0,
            blocksSent: 0
        };

        await write(EpdCmd.RESET_TRANSFER, [this.sessionId]);
        await new Promise(r => setTimeout(r, 100));
        this.log(2, 'Transfer reset, session:', this.sessionId);
    },

    /**
     * Send single block (fast mode, no ACK wait)
     * @param {number} blockId - Block ID
     * @param {number} totalBlocks - Total number of blocks
     * @param {Uint8Array} payload - Data payload
     * @param {boolean} withResponse - Whether to wait for BLE write response
     */
    async sendBlockFast(blockId, totalBlocks, payload, withResponse = false) {
        // Check connection before sending
        if (!this.isConnected()) {
            this.log(1, 'Cannot send block: BLE disconnected');
            throw new Error('BLE disconnected');
        }

        const crc = this.crc16(payload);

        // Calculate cfg byte: low nibble=layer, high nibble=first block flag
        // Block 0 needs 0x00 (send RAM command), other blocks use 0xF0 (continue)
        let cfg;
        if (blockId === 0) {
            // Block 0 always uses first block flag (sends RAM command)
            cfg = 0x00 | (this.currentLayer & 0x0F);
            this.block0Sent = true;
        } else {
            // Other blocks use continue flag
            cfg = 0xF0 | (this.currentLayer & 0x0F);
        }

        // Packet: [cmd][block_id:2][total:2][cfg:1][payload][crc:2]
        const packet = new Uint8Array(8 + payload.length);
        packet[0] = EpdCmd.WRITE_BLOCK;
        packet[1] = blockId & 0xFF;
        packet[2] = blockId >> 8;
        packet[3] = totalBlocks & 0xFF;
        packet[4] = totalBlocks >> 8;
        packet[5] = cfg;
        packet.set(payload, 6);
        packet[6 + payload.length] = crc & 0xFF;
        packet[7 + payload.length] = crc >> 8;

        try {
            // Share ordering, connection checks and the migration-export lock
            // with every other GATT write.
            const sent = await write(EpdCmd.WRITE_BLOCK, packet.subarray(1),
                                     withResponse, false, true);
            if (!sent) throw new Error('BLE write rejected');

            // Update statistics
            this.transferStats.bytesSent += payload.length;
            this.transferStats.blocksSent++;
        } catch (e) {
            this.log(1, `Failed to send block ${blockId}:`, e);
            throw e;
        }
    },

    /**
     * Get list of missing blocks from status
     * @param {Object} status - Transfer status object
     * @param {number} totalBlocks - Total number of blocks
     * @returns {Array<number>} - Array of missing block IDs
     */
    getMissingBlocks(status, totalBlocks) {
        const missing = [];
        const received = status && Number.isFinite(status.received) ? status.received : 0;
        if (!status || totalBlocks <= 0) {
            for (let i = 0; i < totalBlocks; i++) missing.push(i);
            return missing;
        }
        if (received >= totalBlocks) {
            return missing;
        }
        for (let i = received; i < totalBlocks; i++) missing.push(i);
        return missing;
    },

    /**
     * Send image with CRC verification and resume capability
     * @param {Uint8Array} data - Image data to send
     * @param {string} step - 'bw' for black/white, 'red' for color layer
     * @param {function} onProgress - Progress callback (blocksSent, totalBlocks, speedInfo)
     * @returns {Promise<boolean>} True if successful
     */
    async sendImageWithResume(data, step = 'bw', onProgress = null) {
        // Check connection before starting
        if (!this.isConnected()) {
            this.log(1, 'Cannot start transfer: BLE disconnected');
            throw new Error('BLE disconnected');
        }

        let mtu = parseInt(document.getElementById('mtusize').value);
        if (isNaN(mtu) || mtu < 20) {
            this.log(2, 'Invalid MTU value, using default 20');
            mtu = 20;
        }
        const chunkSize = Math.max(mtu - 8, 20); // Account for header/CRC overhead
        const totalBlocks = Math.ceil(data.length / chunkSize);
        if (totalBlocks > 512) {
            this.log(1, `Too many blocks: ${totalBlocks} (MTU=${mtu}, chunk=${chunkSize})`);
            throw new Error(`MTU过小(${mtu})，需要${totalBlocks}块，超过512上限`);
        }

        // Set current layer based on step
        this.currentLayer = (step === 'bw') ? 0x0F : 0x00;

        this.restartRequested = false;
        await this.resetTransfer(Date.now() & 0xFF);

        this.log(2, `Starting transfer: ${totalBlocks} blocks, ${data.length} bytes, layer=${step}`);

        for (let retryRound = 0; retryRound < this.MAX_RETRIES; retryRound++) {
            if (retryRound > 0) {
                this.log(2, 'Restarting sequential stream from block 0');
                this.restartRequested = false;
                await this.resetTransfer(this.sessionId);
            }

            this.log(2, `Round ${retryRound + 1}: sending ${totalBlocks} blocks from 0`);

            for (let blockId = 0; blockId < totalBlocks; blockId++) {
                if (this.restartRequested) {
                    this.log(2, `MCU requested restart at block ${blockId}`);
                    break;
                }
                if (blockId % 10 === 0 && !this.isConnected()) {
                    this.log(1, 'BLE disconnected during transfer');
                    throw new Error('BLE disconnected during transfer');
                }

                const offset = blockId * chunkSize;
                const payload = data.slice(offset, Math.min(offset + chunkSize, data.length));
                if (payload.length === 0) {
                    continue;
                }

                const isLastInBatch = ((blockId + 1) % this.BATCH_SIZE === 0);
                const isLastBlock = (blockId === totalBlocks - 1);
                const useResponse = isLastInBatch || isLastBlock;

                await this.sendBlockFast(blockId, totalBlocks, payload, useResponse);

                if (onProgress) {
                    const speedInfo = this.getTransferSpeed();
                    onProgress(blockId + 1, totalBlocks, speedInfo);
                }
            }

            await new Promise(r => setTimeout(r, this.BATCH_DELAY_MS));

            if (this.restartRequested) {
                continue;
            }

            try {
                const status = await this.queryStatus();
                const stillMissing = this.getMissingBlocks(status, totalBlocks);
                if (stillMissing.length === 0) {
                    const speed = this.getTransferSpeed();
                    this.log(2, `Transfer complete: ${totalBlocks} blocks, ${speed.elapsed}s, ${this.getSpeedString()}`);
                    return true;
                }
                this.log(2, `${stillMissing.length} blocks missing, will restart from 0`);
            } catch (e) {
                this.log(1, 'Post-transfer status query failed:', e);
                if (!this.isConnected()) {
                    throw new Error('BLE disconnected during transfer');
                }
            }
        }

        const speed = this.getTransferSpeed();
        this.log(1, `Transfer failed after ${this.MAX_RETRIES} retries, ${speed.elapsed}s`);
        throw new Error('Transfer failed after max retries');
    },

    /**
     * Set logging level
     * @param {number} level - 0=none, 1=errors, 2=info, 3=debug
     */
    setLogLevel(level) {
        this.logLevel = Math.max(0, Math.min(3, level));
        this.log(2, `Log level set to ${this.logLevel}`);
    },

    /**
     * Initialize the transfer module (call on connect)
     */
    init() {
        this.pendingStatus = null;
        this.statusResolver = null;
        this.statusRequestId = 0;
        this.block0Sent = false;
        this.restartRequested = false;
        this.transferStats = {
            startTime: 0,
            bytesSent: 0,
            blocksSent: 0
        };
        this.log(2, 'Transfer module initialized');
    }
};

// Export for use in main.js
if (typeof window !== 'undefined') {
    window.BleTransfer = BleTransfer;
}
