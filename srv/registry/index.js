const PORT = 5000;
// regex credit: https://stackoverflow.com/a/26445549/13673228
const IPV4_REGEXP = /^(?!0)(?!.*\.$)((1?\d?\d|25[0-5]|2[0-4]\d)(\.|$)){4}$/;

const io = require('socket.io')(PORT);

const allSwitches = new Map();

io.on('connection', function(socket) {
  console.log('[' + new Date() + '] Connected: ' + socket.id);

  socket.on('disconnect', function() {
    console.log('[' + new Date() + '] Disconnected: ' + socket.id);
  });

  let publicIP = socket.request.connection.remoteAddress;

  // for some reason, sigh...
  if (publicIP.startsWith('::ffff:'))
    publicIP = publicIP.substring('::ffff:'.length);

  if (!IPV4_REGEXP.test(publicIP))
    throw new Error(`Unexpected IP ${publicIP}`);

  let initialized = false;
  socket.on('pulse', function(switchIP, vpnPort) {
    if (typeof switchIP !== 'string' || typeof vpnPort !== 'number')
      return;

    if (!IPV4_REGEXP.test(switchIP))
      return;

    if (!Number.isInteger(vpnPort) ||
        vpnPort <= 0 ||
        vpnPort >= 65536)
      return;

    // 20 mins expiry
    const lastPulseAcceptable = Date.now() - 20 * 60 * 1000;

    if (!initialized) {
      initialized = true;

      for (const [localIP, [remoteIP, remotePort, lastPulse]] of allSwitches) {
        if (lastPulse > lastPulseAcceptable)
          socket.emit('set_remote_addr', localIP, remoteIP, remotePort);
        else {
          allSwitches.delete(localIP);
          socket.broadcast.emit('delete_remote_addr', localIP);
        }
      }
    }

    let shouldBroadcast = true;
    if (allSwitches.has(switchIP)) {
      const [publicIPOld, vpnPortOld] = allSwitches.get(switchIP);
      if (publicIPOld === publicIP && vpnPortOld === vpnPort)
        shouldBroadcast = false;
    }

    allSwitches.set(switchIP, [publicIP, vpnPort, Date.now()]);
    if (shouldBroadcast)
      io.sockets.emit('set_remote_addr', switchIP, publicIP, vpnPort);
  });
});
