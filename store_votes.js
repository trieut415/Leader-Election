const dgram = require('dgram');
const path = require('path');
const express = require('express');
const http = require('http');
const socketIo = require('socket.io');
const { Db } = require('tingodb')();

const len_out = 5;
const start_byte = 0x1B;

// Checksum function
function genCheckSum(data) {
    return data.reduce((sum, byte) => sum + byte, 0) % 256;
}

// Initialize Tingodb
const dbPath = path.join(__dirname, 'mydb');
const db = new Db(dbPath, {});
const votesCollection = db.collection('votes');

// Create an express app
const app = express();
const server = http.createServer(app);
const io = socketIo(server);

// Serve static files from the 'public' directory
app.use(express.static('public'));

// Create a UDP socket
const udpServer = dgram.createSocket('udp4');

// Endpoint to get the list of votes
app.get('/votes', function(req, res) {
    // Fetch all votes from the database
    votesCollection.find({}, { _id: 0 }).toArray(function(err, docs) {
        if (err) {
            console.error('Error retrieving votes:', err);
            return res.status(500).send('Internal server error.');
        }
        res.json(docs);
    });
});

// Endpoint to get vote totals
app.get('/vote-totals', function(req, res) {
    votesCollection.find({}, { vote_id: 1, _id: 0 }).toArray(function(err, docs) {
        if (err) {
            console.error('Error retrieving votes:', err);
            return res.status(500).send('Internal server error.');
        }
        const totals = {};
        docs.forEach(function(doc) {
            const voteOption = doc.vote_id;
            if (totals[voteOption] !== undefined) {
                totals[voteOption]++;
            } else {
                totals[voteOption] = 1;
            }
        });
        res.json(totals);
    });
});

// Endpoint to reset the database
app.post('/reset-database', function(req, res) {
    votesCollection.remove({}, { multi: true }, function(err, numRemoved) {
        if (err) {
            console.error('Error resetting database:', err);
            return res.status(500).send('Internal server error.');
        }
        console.log('Database reset. Documents removed:', numRemoved);
        res.status(200).send('Database has been reset.');
    });
});

// Function to log data to the database
function logDataToDatabase(logEntry) {
    // Only store the necessary fields
    const voteData = {
        time: logEntry.time,
        deviceId: logEntry.deviceId,
        votedId: logEntry.votedId
    };

    votesCollection.insert(logEntry, (err, result) => {
        if (err) {
            console.error('Error writing to database', err);
        } else {
            console.log('Data written:', logEntry);
            readAndEmitData();
        }
    });
}

const readAndEmitData = () => {
    votesCollection.find().toArray((err, records) => {
        if (err) {
            console.error('Error reading from database:', err);
            io.emit('data', { error: 'Failed to load data.' });
            return;
        }

        console.log('Raw records from database:', records);

        // Filter out records that do not have the expected vote fields
        const validRecords = records.filter(record => 
            record.time && record.deviceId !== undefined && record.votedId !== undefined
        );

        // Map and structure records to match the UI's expected format
        const formattedData = validRecords.map(record => ({
            time: record.time,
            voter_id: record.deviceId, // Assuming deviceId corresponds to voter_id
            vote_id: record.votedId    // Assuming votedId corresponds to vote_id
        }));

        console.log('Formatted data being emitted:', formattedData);

        // Emit only the structured data
        io.emit('data', formattedData);
    });
};

// Handle incoming UDP messages
udpServer.on('message', (msg, rinfo) => {
    console.log(`Received message from ${rinfo.address}:${rinfo.port}:`, msg);

    const data = Array.from(msg);

    if (data.length !== len_out || data[0] !== start_byte) {
        console.error(`Invalid data format from ${rinfo.address}:${rinfo.port}:`, msg);
        return;
    }

    // Extract fields
    const messageType = data[1];
    const deviceId = data[2];
    const votedId = data[3];
    const receivedChecksum = data[4];

    const validChecksum = genCheckSum(data.slice(0, len_out - 1));
    // if (receivedChecksum !== validChecksum) {
    //     console.error(`Checksum failed from ${rinfo.address}:${rinfo.port}`);
    //     return;
    // }

    // Create log entry
    const logEntry = {
        time: new Date().toISOString(),
        source: `${rinfo.address}:${rinfo.port}`,
        messageType,
        deviceId,
        votedId
    };

    console.log(`Valid message from ${logEntry.source}:`, logEntry);

    // Log data to database and emit to clients
    logDataToDatabase(logEntry);

    // Send confirmation back to the ESP32 device
    const confirmationMessage = Buffer.from('Vote Confirmed');
    udpServer.send(confirmationMessage, rinfo.port, rinfo.address, (err) => {
        if (err) {
            console.error(`Error sending confirmation to ${logEntry.source}:`, err);
        } else {
            console.log(`Confirmation sent to ${logEntry.source}`);
        }
    });
});

// Handle errors
udpServer.on('error', (err) => {
    console.error(`UDP server error:\n${err.stack}`);
    udpServer.close();
});

// Start the UDP server
const UDP_PORT = 4000; // Port for devices to send UDP messages to
udpServer.bind(UDP_PORT, () => {
    console.log(`UDP server listening on port ${UDP_PORT}`);
});

// WebSocket connection for clients (e.g., web interface)
io.on('connection', (socket) => {
    console.log('New client connected to data server');
    readAndEmitData();

    socket.on('disconnect', () => {
        console.log('Client disconnected from data server');
    });
});

io.on('data', (data) => {
    console.log("Received data for UI:", data); // Check the data structure
    if (data.error) {
        console.error('Error receiving data:', data.error);
        return;
    }
    updateVoteList(data);
    updateVoteTotals(data);
});

// Serve static files (same as before)
app.use(express.static(path.join(__dirname, 'public')));

// Start the HTTP server
const PORT = 8080;
server.listen(PORT, () => {
    console.log(`Data server running on port ${PORT}`);
});

// Handle graceful shutdown
process.on('SIGINT', () => {
    console.log('Exiting...');
    udpServer.close();
    server.close();
    process.exit();
});
