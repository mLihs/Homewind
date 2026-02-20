# Homewind UI Demo

Local development environment for testing the Homewind WebUI without flashing to ESP32.

## Quick Start

1. Open `demo.html` in your browser
2. Use the toolbar at the top to simulate different states
3. Edit `../webui_src/app.css` or `../webui_src/app.js` and refresh

## Features

### Demo Toolbar

| Button | Action |
|--------|--------|
| **HR 75/95/120** | Set heart rate value |
| **HR OFF** | Clear heart rate (shows "--") |
| **HR ✓** | Connect HR sensor |
| **HR ✗** | Disconnect HR sensor |
| **CSC ✓** | Connect speed/cadence sensor |
| **+ Fan** | Add a demo fan tile (uses fanManager API) |
| **Active** | Set first fan to ACTIVE state (green) |
| **Inactive** | Set first fan to INACTIVE state (default) |
| **Error** | Set first fan to ERROR state (red) |
| **Console** | Show/hide debug console |
| **Reset** | Reset all demo state |

### Mock System

- **WebSocket**: Mocked, logs to console
- **API Calls**: Returns demo data
- **State**: Managed in `DemoState` object

## File Structure

```
UiDemo/
├── demo.html     # Main demo file
└── README.md     # This file

webui_src/        # Source files (linked from demo.html)
├── app.css       # Styles
├── app.js        # JavaScript
└── index.html    # Production HTML
```

## Tips

1. **Live Reload**: Use VS Code Live Server or similar for auto-reload
2. **DevTools**: Open browser DevTools (F12) for debugging
3. **Console**: Click "Console" button to see mock API/WS logs
4. **State Access**: Type `Demo.getState()` in browser console to inspect state

## Limitations

- WebSocket binary frames not fully simulated
- Some API endpoints return empty success
- Modal dialogs may not work completely without real backend

## Extending

To add more mock functionality, edit the `Demo` object and `fetch` interceptor in `demo.html`.

```javascript
// Example: Add custom mock endpoint
if (url.includes('/api/v1/custom')) {
  return Promise.resolve(new Response(JSON.stringify({
    custom: 'data'
  })));
}
```
