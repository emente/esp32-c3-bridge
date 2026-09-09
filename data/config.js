// preload config from device
document.addEventListener("DOMContentLoaded", async function () {
    const config = await fetch('/config.json').then(r => r.json());

    for (const name in config) {
        const el = document.querySelector("[name='" + name + "']");
        if (el) el.value = config[name];
    }
});

// disable submit button on POST
document.getElementById("config").addEventListener("submit", function () {
    document.getElementById("submit").disabled = true;
});

// import from file
const fileInput = document.getElementById("file");
fileInput.onchange = function () {
    const file = fileInput.files[0];
    if (!file) return;

    const reader = new FileReader();

    reader.onload = function () {
        const config = JSON.parse(reader.result);

        for (const name in config) {
            const el = document.querySelector("[name='" + name + "']");
            if (el) el.value = config[name];
        }
    };

    reader.readAsText(file);

    // allow re-selecting same file later
    fileInput.value = "";
};

// just trigger picker
function importConfig() {
    fileInput.click();
}

// export to file
function exportConfig() {
    const data = new FormData(document.getElementById("config"));
    const config = {};

    for (const [key, value] of data) {
        config[key] = value;
    }

    const a = document.createElement("a");
    a.href = URL.createObjectURL(
        new Blob([JSON.stringify(config)], { type: "application/json" })
    );
    a.download = "config.json";
    a.click();
}
