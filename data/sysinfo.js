fetch("/sysinfo")
.then(r => r.json())
.then(data => {
    const tbody = document.querySelector("#sysinfo tbody");

    tbody.innerHTML = `
        <tr><th colspan="2">System</th></tr>
        <tr><td>ID</td><td>${data.id}</td></tr>
        <tr><td>Version</td><td>${data.version}</td></tr>
        <tr><td>Date/time</td><td>${formatDate(data.datetime)}</td></tr>
        <tr><td>Uptime</td><td>${formatDuration(data.uptime)}</td></tr>

        <tr><th colspan="2">CPU</th></tr>
        <tr><td>Chip</td><td>${data.cpu.chipmodel}</td></tr>
        <tr><td>Frequency</td><td>${data.cpu.frequency} MHz</td></tr>
        <tr><td>Temperature</td><td>${data.cpu.temperature} deg C</td></tr>

        <tr><th colspan="2">Memory</th></tr>
        <tr><td>Total heap</td><td>${data.heap.total}</td></tr>
        <tr><td>Free heap</td><td>${data.heap.free}</td></tr>

        <tr><th colspan="2">Wi-Fi</th></tr>
        <tr><td>SSID</td><td>${data.wifi.ssid}</td></tr>
        <tr><td>BSSID</td><td>${data.wifi.bssid}</td></tr>
        <tr><td>Channel</td><td>${data.wifi.channel}</td></tr>
        <tr><td>Signal</td><td>${data.wifi.rssi} dBm</td></tr>
    `;
});