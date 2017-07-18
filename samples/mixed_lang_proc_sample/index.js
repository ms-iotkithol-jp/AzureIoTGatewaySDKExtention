// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

'use strict';
require('date-utils');

function toUTF8(text) {
    var result = [];
    if (text == null)
        return result;
    for (var i = 0; i < text.length; i++) {
        var c = text.charCodeAt(i);
        if (c <= 0x7f) {
            result.push(c);
        } else if (c <= 0x07ff) {
            result.push(((c >> 6) & 0x1F) | 0xC0);
            result.push((c & 0x3F) | 0x80);
        } else {
            result.push(((c >> 12) & 0x0F) | 0xE0);
            result.push(((c >> 6) & 0x3F) | 0x80);
            result.push((c & 0x3F) | 0x80);
        }
    }
    return result;
}

let ProxyGateway = require('../../proxy/gateway/nodejs/index.js');

// This gateway module simply publishes whatever it receives.
let forward = {
    broker: null,
    macAddress: null,
    create: function(broker) {
        this.broker = broker;
        console.log('broker - ' + broker);
        return true;
    },
    start: function() {
        setInterval(() => {
                var dt = new Date();
                var msg = JSON.stringify({
                    'temperature': Math.random() * 5 + 20,
                    'measuredtime': dt.toFormat("YYYY-MM-DDTHH24:MI:SS")
                });
                console.log('msg is ' + msg);
                this.broker.publish({
                    properties: {
                        'source': 'sensor',
                        'macAddress': process.argv[3]
                    },
                    //content: new Uint8Array(buffer)
                    content: Buffer.from(toUTF8(msg))
                });
            },
            2000);
    },
    receive: function(message) {
        let buf = Buffer.from(message.content);
        console.log(`forward.receive - ${buf.toString('utf8')}`);
        this.broker.publish(message);
    },
    destroy: function() {
        this.broker = null;
    }
};

if (!process.argv[3]) {
    console.log('USAGE: node path/to/app.js <control-id> mac-address');
    process.exit(1);
}

let gateway = new ProxyGateway(forward);
gateway.attach(process.argv[2]);

process.stdin.setRawMode(true);
process.stdin
    .resume()
    .setEncoding('utf8')
    .on('data', function() {
        gateway.detach();
        process.exit();
    });