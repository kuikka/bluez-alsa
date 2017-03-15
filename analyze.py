#!/usr/bin/env python

"""
This script reads the ascii output from 'btmon' and analyzes the SCO data containen mSBC frames.
"""

import sys
import re

SCO_TX = re.compile('.*SCO Data TX.*dlen (\d+).*?(\d+)\.(\d+)')
SCO_RX = re.compile('.*SCO Data RX.*dlen (\d+).*?(\d+)\.(\d+)')
DATA = re.compile('        (..) (..) (..) (..) (..) (..) (..) (..) (..) (..) (..) (..) (..) (..) (..) (..)')

f = open(sys.argv[1], 'r')
lines = f.readlines()

tx_data = []
reading_tx_data = False
tx_data_len = 0;
tx_data_read = 0;
tx_prev = 0
tx_first = 0
tx_bytes = 0

rx_data = []
reading_rx_data = False
rx_data_len = 0;
rx_data_read = 0;
rx_prev = 0

t_marker = 0

for line in lines:
    line = line.rstrip()

    if reading_tx_data:
        d = line[8:55]
        d = d.rstrip()
        d = d.split(' ')
        nr_bytes = len( d )
        tx_data_read += nr_bytes
        for byte in d:
            tx_data.append( int( byte, 16 ) )
        if tx_data_read >= tx_data_len:
            reading_tx_data = False
    elif reading_rx_data:
        d = line[8:55]
        d = d.rstrip()
        d = d.split(' ')
        nr_bytes = len( d )
        rx_data_read += nr_bytes
        for byte in d:
            rx_data.append( int( byte, 16 ) )
        if rx_data_read >= rx_data_len:
            reading_rx_data = False

    else:
        m = SCO_TX.match(line)
        if m:
#            print line
            tx_data_len = int(  m.group(1) )
#            print '%d.%d' % (int(m.group(2)), int(m.group(3)))
            t = float( int(m.group(2)) * 1000000 + int(m.group(3)) )
            delta_t = (t - tx_prev)/1000
            marker = ''
            threshold = 27
            if delta_t > threshold:
                marker = 'xxxxxx'
            print 'Time %f delta %f milliseconds. Total %f bytes/second %s %f' % (t/1000, delta_t, tx_bytes/((t - tx_first)/1000000), marker, t - t_marker )
            if delta_t > threshold:
                t_marker = t/1000
            tx_bytes += tx_data_len
            tx_prev = t
            if tx_first == 0:
                tx_first = t
            tx_data_read = 0
            reading_tx_data = True
            continue
        m = SCO_RX.match(line)
        if m:
#            print line
            rx_data_len = int(  m.group(1) )
#            print '%d.%d' % (int(m.group(2)), int(m.group(3)))
            t = float( int(m.group(2)) * 1000000 + int(m.group(3)) )
#            print 'Time %f delta %f milliseconds' % (t/1000, (t - rx_prev)/1000)
            rx_prev = t
            rx_data_read = 0
            reading_rx_data = True
            continue

#print len( tx_data )
#print len( rx_data )
#print data

def get_frame_number( d ):
    if d == 0x08:
        return 0
    if d == 0x38:
        return 1
    if d ==0xc8:
        return 2
    if d == 0xf8:
        return 3

    return -1

out_file = open('output.msbc', 'wb')

i = 0
nr_frames = 0
while i < len( tx_data ):
    if tx_data[i] == 0x01 and tx_data[i+2] == 0xad and tx_data[i+3] == 0x00 and tx_data[i+4] == 0x00:
        print 'frame at %d nr %d CRC %02x' % ( i, get_frame_number( tx_data[i + 1] ), tx_data[i + 5] )
        nr_frames = nr_frames + 1
        frame = bytearray( tx_data[i+2:i+2+57] )
        out_file.write(frame)

    i = i + 1

print 'Found %d frames in %d bytes, %d bytes padding' % ( nr_frames, len(tx_data), len(tx_data) - (nr_frames * 59) )
