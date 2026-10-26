// Cross-origin isolation for a static host (GitHub Pages sends no COOP/COEP
// headers).  jwemu blocks its worker in Atomics.wait on a SharedArrayBuffer,
// and SharedArrayBuffer exists only on a cross-origin-isolated page, so this
// service worker re-serves every same-origin response with the two headers.
//
// The same file is the page-side registration script: loaded by a <script>
// tag it registers itself and reloads once so the page comes under it.
if (typeof window === 'undefined') {
    self.addEventListener('install', () => self.skipWaiting());
    self.addEventListener('activate', (e) => e.waitUntil(self.clients.claim()));
    self.addEventListener('fetch', (e) => {
        const req = e.request;
        if (req.cache === 'only-if-cached' && req.mode !== 'same-origin') return;
        e.respondWith(fetch(req).then((res) => {
            if (res.status === 0) return res;
            const headers = new Headers(res.headers);
            headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
            headers.set('Cross-Origin-Opener-Policy', 'same-origin');
            headers.set('Cross-Origin-Resource-Policy', 'same-origin');
            return new Response(res.body, {status: res.status, statusText: res.statusText, headers});
        }).catch((err) => new Response(String(err), {status: 500})));
    });
} else if (!window.crossOriginIsolated && 'serviceWorker' in navigator) {
    navigator.serviceWorker.register(document.currentScript.src).then((reg) => {
        const reload = () => {
            if (sessionStorage.getItem('jwemu-coi-reloaded')) return;
            sessionStorage.setItem('jwemu-coi-reloaded', '1');
            location.reload();
        };
        if (reg.active && !navigator.serviceWorker.controller) reload();
        navigator.serviceWorker.addEventListener('controllerchange', reload);
    });
}
