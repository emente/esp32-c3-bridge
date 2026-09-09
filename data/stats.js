
async function loadStats() {
    const response = await fetch('/stats');

    if (!response.ok) {
        throw new Error(`HTTP ${response.status}`);
    }

    const stats = await response.json();

    const counts = stats.counts;
    const max = Math.max(...counts, 1); // avoid division by zero

    const chart = document.getElementById('chart');

    const total = counts.length;

    chart.innerHTML = [...counts]
        .reverse()
        .map((count, index) => {
            const minute = total - 1 - index;

            return `
                <div
                    class="bar"
                    title="-${minute} min: ${count} messages"
                    style="height:${Math.max(2, (count / max) * 100)}%">
                </div>
            `;
        }
    ).join('');
    document.getElementById('y-max').textContent = max;
    document.getElementById('latest').textContent = formatDate(stats.latest);
}

loadStats().catch(error => {
    console.error('Failed to load stats:', error);
});
