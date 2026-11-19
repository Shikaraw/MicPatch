// UI portion of App; included inside the implementation namespace in main.cpp.
struct Palette {
    COLORREF bg, deep, panel, raised, line, ink, sub, dim, mint, warn, danger;
    COLORREF accentText, accentPressed, meterHigh;
};

constexpr Palette paletteFor(micpatch::Theme theme) {
    switch (theme) {
    case micpatch::Theme::Ocean:
        return {RGB(13,24,39), RGB(9,18,30), RGB(20,35,55), RGB(29,51,77), RGB(55,82,112),
                RGB(237,245,255), RGB(176,202,228), RGB(136,163,192), RGB(120,205,255),
                RGB(255,190,178), RGB(66,43,55), RGB(9,22,38), RGB(87,177,232), RGB(176,220,146)};
    case micpatch::Theme::Light:
        return {RGB(235,241,245), RGB(248,251,253), RGB(255,255,255), RGB(222,234,242), RGB(161,184,198),
                RGB(23,39,51), RGB(62,83,98), RGB(91,111,125), RGB(0,104,119),
                RGB(152,54,40), RGB(255,233,228), RGB(255,255,255), RGB(0,79,92), RGB(103,119,29)};
    case micpatch::Theme::Graphite:
    default:
        return {RGB(20,25,27), RGB(16,21,23), RGB(27,34,36), RGB(35,46,47), RGB(55,70,72),
                RGB(237,244,242), RGB(173,191,188), RGB(133,153,151), RGB(117,230,197),
                RGB(255,188,169), RGB(62,43,42), RGB(16,21,23), RGB(90,193,164), RGB(201,221,141)};
    }
}

struct SourceRow {
    HWND select{}, gain{}, mute{}, play{}, loop{};
    RECT card{}, meter{}, value{};
    int peak{};
    bool visible{}, missing{}, failed{};
};

struct App : private Palette {
    HINSTANCE instance;
    HWND window{}, add{}, refresh{}, sourceDevice{}, removeSource{}, voiceEq{}, ducking{};
    HWND filePath{}, chooseFileButton{}, outputDevice{}, sourceScroll{};
    HWND outMute{}, start{}, stop{}, status{}, tooltip{}, virtualDevice{}, settingsButton{};
    std::array<SourceRow, micpatch::maxSources> rows{};
    RECT banner{}, bus{}, master{}, inspector{}, footer{}, outMeter{}, emptyCard{};
    HFONT font{}, titleFont{}, smallFont{}, boldFont{};
    HICON brandIcon{};
    int brandSize{};
    micpatch::Settings preferences;
    HBRUSH panelBrush = CreateSolidBrush(panel), deepBrush = CreateSolidBrush(deep);
    UINT dpi{96};
    micpatch::Engine engine;
    std::vector<micpatch::Endpoint> microphones, outputs, loopbackDevices;
    std::vector<micpatch::SourceConfig> sources;
    std::wstring outputId, statusText;
    size_t detail{kNoSource};
    int outPeak{}, firstRow{}, rowPage{1}, wheelRemainder{};
    bool active{}, starting{}, statusError{}, muteHotkeyRegistered{}, virtualRefreshPending{};
    bool autoSave{}, saveDirty{};
    LSTATUS saveError{};
    DWORD muteHotkeyError{};
    ULONGLONG startTick{}, saveAfter{};

    explicit App(HINSTANCE module, micpatch::Settings initial)
        : Palette(paletteFor(initial.theme)), instance(module), preferences(std::move(initial)),
          autoSave(micpatch::autoSaveEnabled()) {
        engine.outMute.store(preferences.startMuted);
        if (autoSave) {
            auto session = micpatch::loadSession();
            sources = std::move(session.sources);
            if (preferences.rememberOutput) outputId = std::move(session.outputId);
        }
        if (preferences.rememberOutput && outputId.empty()) outputId = preferences.outputId;
        if (!sources.empty()) detail = 0;
    }
    HBRUSH controlColors(HDC dc, HWND child, bool listBox) const {
        const bool isStatus = child == status;
        SetTextColor(dc, isStatus && statusError ? warn : isStatus ? sub : ink);
        SetBkColor(dc, isStatus || listBox ? deep : panel);
        return isStatus || listBox ? deepBrush : panelBrush;
    }
    ~App() {
        for (auto f : {font, titleFont, smallFont, boldFont}) if (f) DeleteObject(f);
        if (panelBrush) DeleteObject(panelBrush);
        if (deepBrush) DeleteObject(deepBrush);
        if (brandIcon) DestroyIcon(brandIcon);
    }
    int px(int dip) const { return MulDiv(dip, static_cast<int>(dpi), 96); }
    RECT box(int x, int y, int w, int h) const { return {px(x),px(y),px(x+w),px(y+h)}; }
    RECT inset(RECT r, int x, int y) const { InflateRect(&r, -px(x), -px(y)); return r; }
    RECT part(RECT r, int x, int y, int right, int height) const {
        return {r.left+px(x),r.top+px(y),r.right-px(right),r.top+px(y+height)};
    }
    static void fill(HDC dc, const RECT& r, COLORREF color) {
        SetDCBrushColor(dc, color);
        FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    void rounded(HDC dc, RECT r, COLORREF color, COLORREF border, int radius = 10) const {
        const auto brush = SelectObject(dc, GetStockObject(DC_BRUSH));
        const auto pen = SelectObject(dc, GetStockObject(DC_PEN));
        SetDCBrushColor(dc, color); SetDCPenColor(dc, border);
        RoundRect(dc, r.left, r.top, r.right, r.bottom, px(radius), px(radius));
        SelectObject(dc, pen); SelectObject(dc, brush);
    }
    static constexpr UINT single = DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
    void text(HDC dc, std::wstring_view str, RECT r, COLORREF color,
              UINT flags = single, HFONT face = nullptr) const {
        if (str.empty()) return;
        const auto old = SelectObject(dc, face ? face : font ? font : GetStockObject(DEFAULT_GUI_FONT));
        SetTextColor(dc, color); SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, str.data(), static_cast<int>(str.size()), &r, flags | DT_NOPREFIX);
        SelectObject(dc, old);
    }
    static std::wstring caption(HWND hwnd) {
        const int n = GetWindowTextLengthW(hwnd);
        std::wstring result(static_cast<size_t>(n) + 1, L'\0');
        result.resize(GetWindowTextW(hwnd, result.data(), n + 1));
        return result;
    }
    HWND control(const wchar_t* cls, const wchar_t* title, DWORD style = 0, int id = 0) const {
        return CreateWindowExW(0, cls, title, WS_CHILD | WS_VISIBLE | style, 0,0,0,0, window,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    }
    HWND button(const wchar_t* title, int id) const {
        return control(L"BUTTON", title, WS_TABSTOP | BS_OWNERDRAW, id);
    }
    HWND combo(int id) {
        HWND hwnd = control(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST |
                            CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, id);
        if (hwnd) SetWindowSubclass(hwnd, comboProc, 1, reinterpret_cast<DWORD_PTR>(this));
        return hwnd;
    }
    void setFont() {
        if (brandSize != px(40)) {
            if (brandIcon) DestroyIcon(brandIcon);
            brandSize = px(40);
            brandIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_MICPATCH), IMAGE_ICON,
                brandSize, brandSize, 0));
        }
        const auto make = [&](int size, int weight) {
            return CreateFontW(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        };
        const auto oldFont = font, oldTitle = titleFont, oldSmall = smallFont, oldBold = boldFont;
        font = make(14, FW_NORMAL); titleFont = make(22, FW_SEMIBOLD);
        smallFont = make(12, FW_NORMAL); boldFont = make(15, FW_SEMIBOLD);
        const HFONT use = font ? font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(use), TRUE);
        for (HWND cb : {sourceDevice, outputDevice}) {
            SendMessageW(cb, CB_SETITEMHEIGHT, 0, px(27));
            SendMessageW(cb, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), px(28));
        }
        if (tooltip) {
            SendMessageW(tooltip, WM_SETFONT, reinterpret_cast<WPARAM>(use), TRUE);
            SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, px(520));
        }
        for (auto f : {oldFont, oldTitle, oldSmall, oldBold}) if (f) DeleteObject(f);
    }
    bool createControls() {
        start = button(tr(L"启动发送", L"Start"), Start); stop = button(tr(L"停止发送", L"Stop"), Stop);
        outMute = button(engine.outMute.load() ? tr(L"取消总静音", L"Unmute All") :
                                                 tr(L"总静音", L"Mute All"), OutMute);
        outputDevice = combo(OutputDevice);
        refresh = button(tr(L"刷新设备", L"Refresh"), Refresh); virtualDevice = button(tr(L"虚拟麦克风", L"Virtual Mic"), VirtualDevice);
        settingsButton = button(tr(L"设置", L"Settings"), OpenSettings);
        add = button(tr(L"＋ 添加音源", L"+ Add Source"), Add);
        for (size_t i = 0; i < rows.size(); ++i) {
            auto& row = rows[i];
            row.select = button(L"", rowId(i, SelectRow));
            row.mute = button(tr(L"静音", L"Mute"), rowId(i, MuteRow));
            row.gain = control(TRACKBAR_CLASSW, tr(L"增益", L"Gain"), WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, rowId(i, GainRow));
            row.play = button(tr(L"播放", L"Play"), rowId(i, PlayRow));
            row.loop = button(tr(L"循环播放", L"Loop"), rowId(i, LoopRow));
            for (auto child : {row.select, row.mute, row.gain, row.play, row.loop}) {
                if (!child) return false;
                ShowWindow(child, SW_HIDE);
            }
            SendMessageW(row.gain, TBM_SETRANGE, FALSE, MAKELPARAM(0, 200));
            SendMessageW(row.gain, TBM_SETPAGESIZE, 0, 10);
        }
        sourceScroll = control(L"SCROLLBAR", L"", SBS_VERT | WS_TABSTOP, ScrollSources);
        sourceDevice = combo(SourceDevice);
        filePath = control(L"STATIC", L"", SS_CENTERIMAGE | SS_PATHELLIPSIS | SS_NOTIFY | SS_NOPREFIX);
        chooseFileButton = button(tr(L"更换文件…", L"Change File…"), ChooseFile);
        voiceEq = button(tr(L"人声清晰 EQ", L"Voice clarity EQ"), VoiceEq);
        ducking = button(tr(L"自动压低此音源", L"Auto-duck this source"), Ducking);
        removeSource = button(tr(L"移除音源", L"Remove Source"), RemoveSource);
        status = control(L"STATIC", L"", SS_LEFT | SS_NOPREFIX);
        for (HWND child : {start, stop, outMute, outputDevice, refresh, virtualDevice, settingsButton, add,
                           sourceScroll, sourceDevice, filePath, chooseFileButton, voiceEq, ducking, removeSource, status})
            if (!child) return false;
        tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP |
                                  TTS_NOPREFIX, CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,
                                  window, nullptr, instance, nullptr);
        if (tooltip) {
            TOOLINFOW tool{};
            tool.cbSize = sizeof(tool); tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            tool.hwnd = window; tool.uId = reinterpret_cast<UINT_PTR>(filePath);
            tool.lpszText = LPSTR_TEXTCALLBACKW;
            SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        }
        setFont();
        return true;
    }
    static void showControl(HWND hwnd, bool visible) {
        if (((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_VISIBLE) != 0) != visible)
            ShowWindow(hwnd, visible ? SW_SHOWNA : SW_HIDE);
    }
    void place(HWND hwnd, int x, int y, int width, int height) const {
        if (hwnd) MoveWindow(hwnd, px(x), px(y), px(std::max(0,width)), px(height), FALSE);
    }
    void layout() {
        RECT client{}; GetClientRect(window, &client);
        const int w = MulDiv(client.right, 96, static_cast<int>(dpi));
        const int h = MulDiv(client.bottom, 96, static_cast<int>(dpi));
        const int side = 20, full = w - 2*side;
        const int inspectorW = std::clamp(full*36/100, 246, 360);
        const int leftW = full-inspectorW-14, rightX = side+leftW+14;
        const int busLeft = full*56/100;
        banner = box(0,0,w,64); bus = box(side,78,full,138);
        master = box(side+busLeft+16,91,full-busLeft-32,111);
        outMeter = box(side+busLeft+16,142,full-busLeft-32,18);
        inspector = box(rightX,230,inspectorW,std::max(318,h-310));
        footer = box(0,h-70,w,70);
        place(settingsButton,w-506,15,78,34);
        place(start,w-298,15,88,34); place(stop,w-202,15,88,34); place(outMute,w-106,15,86,34);
        place(virtualDevice,side+busLeft-128,137,116,27);
        place(outputDevice,side+14,168,busLeft-122,224); place(refresh,side+busLeft-99,168,87,32);
        place(add,side+leftW-124,231,124,34);
        constexpr int top = 277, pitch = 134, height = 124;
        rowPage = std::max(1, (h - 70 - top + (pitch - height)) / pitch);
        const int count = static_cast<int>(sources.size());
        firstRow = std::clamp(firstRow, 0, std::max(0, count - rowPage));
        const bool scrollable = count > rowPage;
        const int cardW = leftW - (scrollable ? 23 : 0);
        emptyCard = box(side,top,leftW,height);
        const HWND focus = GetFocus();
        bool moveFocus = focus == sourceScroll && !scrollable;
        for (size_t i = 0; i < rows.size(); ++i) {
            auto& row = rows[i];
            const int slot = static_cast<int>(i) - firstRow;
            row.visible = i < sources.size() && slot >= 0 && slot < rowPage;
            const bool file = row.visible && sources[i].kind == SourceKind::File;
            if (!row.visible && (focus == row.select || focus == row.mute || focus == row.gain ||
                                 focus == row.play || focus == row.loop)) moveFocus = true;
            if (!file && (focus == row.play || focus == row.loop)) moveFocus = true;
            if (row.visible) {
                const int y = top + slot*pitch;
                row.card = box(side,y,cardW,height);
                row.meter = box(side+14,y+52,cardW-28,15);
                row.value = box(side+cardW-65,y+68,51,24);
                place(row.select,side+8,y+5,cardW-96,42); place(row.mute,side+cardW-78,y+10,64,31);
                place(row.gain,side+61,y+68,cardW-134,24);
                if (file) {
                    place(row.play,side+14,y+92,74,26);
                    place(row.loop,side+98,y+92,112,26);
                }
            }
            for (auto child : {row.select, row.mute, row.gain}) showControl(child, row.visible);
            showControl(row.play, file); showControl(row.loop, file);
        }
        if (scrollable) {
            SCROLLINFO scroll{sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS,
                              0, count-1, static_cast<UINT>(rowPage), firstRow, 0};
            SetScrollInfo(sourceScroll, SB_CTL, &scroll, TRUE);
            place(sourceScroll,side+leftW-16,top,16,(rowPage-1)*pitch+height);
        }
        showControl(sourceScroll, scrollable);
        if (moveFocus) SetFocus(scrollable ? sourceScroll : count ? rows[static_cast<size_t>(firstRow)].select : refresh);
        const int rx = rightX+15, rw = inspectorW-30;
        place(sourceDevice,rx,325,rw,218); place(filePath,rx+7,325,rw-14,32);
        place(chooseFileButton,rx,365,rw,32);
        place(voiceEq,rx,369,rw,34); place(ducking,rx,405,rw,34);
        place(removeSource,rx,510,rw,30);
        place(status,20,h-60,w-40,38);
        RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE | RDW_ALLCHILDREN);
    }

    void stroke(HDC dc, const POINT* pts, int count, COLORREF color, int width = 1) const {
        const HPEN pen = CreatePen(PS_SOLID, px(width), color);
        if (!pen) return;
        const auto old = SelectObject(dc, pen);
        Polyline(dc, pts, count);
        SelectObject(dc, old); DeleteObject(pen);
    }
    void glyph(HDC dc, RECT r, int kind) const {
        rounded(dc,r,raised,raised,10);
        const int x = (r.left+r.right)/2, y = (r.top+r.bottom)/2;
        if (kind == 0) {
            constexpr int heights[]{6,12,20,14,8};
            for (int i=0; i<5; ++i) {
                const int dx = x+px((i-2)*5), dy = px(heights[i])/2;
                fill(dc,{dx-px(1),y-dy,dx+px(1),y+dy},mint);
            }
        } else if (kind == 1) {
            rounded(dc,{x-px(4),y-px(9),x+px(4),y+px(3)},mint,mint,7);
            const POINT pts[]{{x-px(7),y},{x-px(7),y+px(3)},{x-px(4),y+px(6)},
                              {x+px(4),y+px(6)},{x+px(7),y+px(3)},{x+px(7),y}};
            stroke(dc,pts,6,mint);
            fill(dc,{x-px(1),y+px(6),x+px(1),y+px(10)},mint);
            fill(dc,{x-px(4),y+px(9),x+px(4),y+px(10)},mint);
        } else if (kind == 3) {
            const POINT speaker[]{{x-px(9),y-px(4)},{x-px(5),y-px(4)},{x,y-px(8)},
                {x,y+px(8)},{x-px(5),y+px(4)},{x-px(9),y+px(4)},{x-px(9),y-px(4)}};
            const POINT waves[]{{x+px(4),y-px(6)},{x+px(8),y-px(3)},{x+px(8),y+px(3)},{x+px(4),y+px(6)}};
            stroke(dc,speaker,7,mint,2); stroke(dc,waves,4,mint);
        } else {
            const POINT pts[]{{x,y+px(4)},{x,y-px(8)},{x+px(7),y-px(6)},{x+px(7),y-px(2)}};
            stroke(dc,pts,4,mint,2);
            rounded(dc,{x-px(6),y+px(2),x+px(2),y+px(9)},mint,mint,7);
        }
    }
    static std::wstring db(int peak) {
        if (!peak) return L"−∞ dBFS";
        const int v = static_cast<int>(std::lround(-200.0 * std::log10(peak/10000.0)));
        return (v ? L"−" : L"") + std::to_wstring(v/10) + L"." + std::to_wstring(v%10) + L" dBFS";
    }
    void meter(HDC dc, RECT r, int peak, int segments) const {
        const int width = r.right-r.left;
        const int lit = peak ? std::clamp(static_cast<int>(std::lround(segments *
                            (1.0 + std::log10(peak/10000.0)/3.0))),0,segments) : 0;
        for (int i=0; i<segments; ++i) {
            RECT cell{r.left + width*i/segments,r.top,r.left + width*(i+1)/segments-px(2),r.bottom};
            fill(dc,cell,i>=lit ? raised : i>=segments-1 ? warn : i>=segments-3 ? meterHigh : mint);
        }
    }
    void drawMeter(HDC dc, const SourceRow& row) const {
        RECT track = row.meter; track.right -= px(94);
        meter(dc,track,row.peak,25);
        text(dc,db(row.peak),{track.right+px(7),track.top,row.meter.right,track.bottom},sub,
             single|DT_RIGHT,smallFont);
    }
    std::wstring routeName() const {
        if (const auto* endpoint = findEndpoint(outputs, outputId)) return endpoint->name;
        return outputId.empty() ? tr(L"尚未选择输出", L"No output selected") : tr(L"输出设备缺失", L"Output device missing");
    }
    void drawScene(HDC dc, const RECT& client) const {
        fill(dc,client,bg); fill(dc,banner,deep);
        fill(dc,{banner.left,banner.bottom-1,banner.right,banner.bottom},line);
        if (brandIcon) DrawIconEx(dc, px(20), px(12), brandIcon, brandSize, brandSize, 0, nullptr, DI_NORMAL);
        else glyph(dc,box(20,12,40,40),0);
        text(dc,tr(L"声搭", L"MicPatch"),box(71,8,152,29),ink,single,titleFont);
        text(dc,tr(L"MicPatch  /  混音台", L"Audio Mixer"),box(72,37,151,18),sub,single,smallFont);
        const bool muted = engine.outMute.load();
        const wchar_t* state = muted ? tr(L"已总静音", L"Muted") : starting ? tr(L"正在启动", L"Starting") : active ? tr(L"正在发送", L"Sending") : tr(L"未发送", L"Not sending");
        const COLORREF stateColor = muted ? warn : active ? mint : sub;
        RECT stateBox{banner.right-px(420),px(18),banner.right-px(311),px(46)};
        rounded(dc,stateBox,panel,line,20);
        rounded(dc,part(stateBox,11,11,91,6),stateColor,stateColor,7);
        text(dc,state,part(stateBox,24,0,6,28),stateColor,single,smallFont);

        rounded(dc,bus,panel,line,14);
        RECT route = bus; route.right = master.left-px(25);
        const bool virtualMic = outputId == kVirtualMicOutputId;
        text(dc,virtualMic ? tr(L"通话软件里选择这个麦克风", L"SELECT THIS MIC IN YOUR CALL APP") : tr(L"通话路由  /  输出设备", L"CALL ROUTING  /  OUTPUT DEVICE"),
             part(route,14,10,4,18),mint,single,smallFont);
        text(dc,routeName(),part(route,14,29,4,29),ink,single,titleFont);
        text(dc,virtualMic ? tr(L"内置虚拟麦克风 · 无需虚拟音频线", L"Built-in mic · No virtual cable") : tr(L"选择声搭发送到的输出端", L"Choose the output endpoint"),
             part(route,14,61,130,19),sub,single,smallFont);
        fill(dc,{master.left-px(16),master.top+px(3),master.left-px(16)+1,master.bottom-px(3)},line);
        text(dc,tr(L"总输出", L"Master Output"),part(master,0,4,0,24),ink,single,boldFont);
        text(dc,db(outPeak),part(master,0,4,0,24),mint,single|DT_RIGHT,boldFont);
        meter(dc,outMeter,outPeak,30);
        constexpr const wchar_t* scale[]{L"−60",L"−45",L"−30",L"−15",L"0"};
        for (int i=0; i<5; ++i) {
            const int x = outMeter.left + (outMeter.right-outMeter.left)*i/4;
            RECT r{x-px(i ? 18 : 0),outMeter.bottom+px(6),x+px(i==4 ? 0 : 25),outMeter.bottom+px(25)};
            text(dc,scale[i],r,sub,single|(i==4 ? DT_RIGHT : i ? DT_CENTER : DT_LEFT),smallFont);
        }
        text(dc,tr(L"音源", L"Sources"),box(20,229,85,37),ink,single,titleFont);
        text(dc,std::to_wstring(sources.size())+L" / "+std::to_wstring(micpatch::maxSources),box(108,232,72,35),sub,single,smallFont);
        for (size_t i = 0; i < sources.size(); ++i) {
            const auto& row = rows[i];
            if (!row.visible) continue;
            const bool picked = detail == i;
            rounded(dc,row.card,panel,picked ? mint : line,13);
            if (picked) fill(dc,{row.card.left+1,row.card.top+px(15),
                                 row.card.left+px(3),row.card.bottom-px(15)},mint);
            drawMeter(dc,row);
            text(dc,tr(L"增益", L"Gain"),part(row.card,14,68,0,24),sub,single,smallFont);
            text(dc,std::to_wstring(sources[i].gain)+L"%",row.value,ink,single|DT_RIGHT,smallFont);
        }
        if (sources.empty()) {
            rounded(dc,emptyCard,panel,line,13);
            text(dc,tr(L"还没有音源", L"No sources yet"),part(emptyCard,16,15,16,29),ink,single,boldFont);
            text(dc,tr(L"可添加麦克风、本地音频文件和播放回环。", L"Add microphones, local audio files, or playback loopback."),
                 part(emptyCard,16,49,16,55),sub,DT_WORDBREAK,smallFont);
        }

        rounded(dc,inspector,panel,line,14);
        text(dc,tr(L"音源设置", L"SOURCE SETTINGS"),part(inspector,15,12,15,19),mint,single,smallFont);
        const bool selected = detail < sources.size();
        text(dc,selected ? sourceKindName(sources[detail].kind) : tr(L"未选中音源", L"No Source Selected"),
             part(inspector,15,32,15,33),ink,single,titleFont);
        if (selected) {
            const auto kind = sources[detail].kind;
            const bool file = kind == SourceKind::File, loopback = kind == SourceKind::Loopback;
            text(dc,file ? tr(L"音频文件", L"Audio file") : loopback ? tr(L"播放设备", L"Playback device") : tr(L"录音设备", L"Recording device"),
                 part(inspector,15,70,15,20),sub,single,smallFont);
            if (file) rounded(dc,part(inspector,15,94,15,34),panel,line);
            const wchar_t* hint = loopback
                ? tr(L"回环捕获该设备的全部播放声音，可能包含通话对方。请选择与发送输出不同的设备。",
                     L"Loopback captures all playback on this device, including callers. Pick a different device from the sending output.")
                : rows[detail].missing || rows[detail].failed
                    ? tr(L"此音源不可用。停止发送后可刷新设备或更换来源。", L"This source is unavailable. Stop sending to refresh devices or change the source.")
                    : file ? tr(L"启动发送后，使用卡片上的播放和循环按钮。", L"Start sending, then use Play and Loop on the source card.")
                           : tr(L"停止发送后可更换或移除此音源。", L"Stop sending to change or remove this source.");
            text(dc,hint,part(inspector,15,216,15,60),loopback || rows[detail].missing || rows[detail].failed ? warn : sub,DT_WORDBREAK,smallFont);
        } else {
            text(dc,tr(L"添加音源后，选择左侧卡片可查看设备、文件及混音设置。", L"Select a source card to see its device, file, and mixing options."),
                 part(inspector,15,75,15,78),sub,DT_WORDBREAK);
        }
        fill(dc,footer,deep);
        fill(dc,{footer.left,footer.top,footer.right,footer.top+1},line);
        std::wstring hotkey = muteHotkeyName(preferences.hotkey);
        const bool disabled = preferences.hotkey == micpatch::MuteHotkey::Off;
        if (disabled) hotkey = tr(L"总静音热键已关闭。", L"Mute hotkey is off.");
        else if (muteHotkeyRegistered) hotkey += tr(L"  切换总静音", L"  Toggle master mute");
        else if (muteHotkeyError == ERROR_HOTKEY_ALREADY_REGISTERED) {
            hotkey += tr(L" 已占用（1409，占用进程未知）。可手动总静音。", L" is in use (1409; owner unknown). Use the mute button instead.");
        } else {
            hotkey += tr(L" 注册失败（错误 ", L" registration failed (error ") + std::to_wstring(muteHotkeyError) +
                      tr(L"）。可手动总静音。", L"). Use the mute button instead.");
        }
        if (saveError != ERROR_SUCCESS) hotkey = tr(L"无法自动保存会话（错误 ", L"Autosave failed (error ") +
                                                 std::to_wstring(saveError) + tr(L"）。", L").");
        text(dc,hotkey,part(footer,20,48,20,18),saveError == ERROR_SUCCESS && (disabled || muteHotkeyRegistered) ? sub : warn,single,smallFont);
    }
    void paint() const {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(window,&ps);
        RECT client{}; GetClientRect(window,&client);
        const int width = ps.rcPaint.right-ps.rcPaint.left, height = ps.rcPaint.bottom-ps.rcPaint.top;
        if (width > 0 && height > 0) {
            HDC buffer = CreateCompatibleDC(dc);
            HBITMAP bmp = buffer ? CreateCompatibleBitmap(dc,width,height) : nullptr;
            if (bmp) {
                const auto old = SelectObject(buffer,bmp);
                SetViewportOrgEx(buffer,-ps.rcPaint.left,-ps.rcPaint.top,nullptr);
                drawScene(buffer,client);
                BitBlt(dc,ps.rcPaint.left,ps.rcPaint.top,width,height,buffer,ps.rcPaint.left,ps.rcPaint.top,SRCCOPY);
                SelectObject(buffer,old); DeleteObject(bmp);
            } else drawScene(dc,client);
            if (buffer) DeleteDC(buffer);
        }
        EndPaint(window,&ps);
    }
    static size_t controlRow(int id) {
        return id < FirstSourceControl ? kNoSource : static_cast<size_t>(id - FirstSourceControl) / RowControlCount;
    }
    static int rowAction(int id) { return (id - FirstSourceControl) % RowControlCount; }
    bool toggleState(int id) const {
        if (id == OutMute) return engine.outMute.load();
        if (detail < sources.size()) {
            if (id == VoiceEq) return sources[detail].voiceEq;
            if (id == Ducking) return sources[detail].ducking;
        }
        const auto index = controlRow(id);
        if (index < sources.size()) {
            if (rowAction(id) == MuteRow) return sources[index].mute;
            if (rowAction(id) == LoopRow) return sources[index].loop;
        }
        return false;
    }
    void drawButton(const DRAWITEMSTRUCT& item) const {
        const auto id = static_cast<int>(item.CtlID);
        const bool disabled = (item.itemState & ODS_DISABLED) != 0;
        const bool pressed = (item.itemState & ODS_SELECTED) != 0;
        const bool focus = (item.itemState & ODS_FOCUS) != 0;
        const bool on = toggleState(id);
        const auto r = item.rcItem;
        const auto index = controlRow(id);
        const auto action = index < sources.size() ? rowAction(id) : -1;
        if (action == SelectRow) {
            const auto kind = sources[index].kind;
            const auto bgColor = pressed ? raised : panel;
            fill(item.hDC,r,panel);
            rounded(item.hDC,inset(r,1,1),bgColor,focus ? mint : bgColor);
            glyph(item.hDC,{r.left+px(6),r.top+px(4),r.left+px(40),r.top+px(38)},
                  kind == SourceKind::Microphone ? 1 : kind == SourceKind::File ? 2 : 3);
            const bool unavailable = rows[index].missing || rows[index].failed;
            text(item.hDC,sourceName(index),part(r,49,0,6,22),ink,single,boldFont);
            text(item.hDC,sourceDescription(index),part(r,49,21,6,17),unavailable ? warn : sub,single,smallFont);
            return;
        }
        const COLORREF base = id==Start || id==Stop || id==OutMute ? deep :
                              id==Refresh || id==Add ? (id==Add ? bg : panel) : panel;
        const bool primary = !disabled && (id==Start || (id==OutMute && !on));
        const bool warm = on && (action==MuteRow || id==OutMute);
        const COLORREF face = disabled ? base : primary ? (pressed ? accentPressed : mint) :
                              warm ? danger : pressed || on ? raised : panel;
        const COLORREF edge = focus ? (primary ? ink : mint) : disabled ? line :
                              primary ? mint : warm ? warn : on ? mint : line;
        const COLORREF fg = disabled ? dim : primary ? accentText : warm ? warn : on ? mint : ink;
        fill(item.hDC,r,base);
        rounded(item.hDC,inset(r,1,1),face,edge);
        auto title = caption(item.hwndItem);
        if (action==MuteRow) title = on ? tr(L"已静音", L"Muted") : tr(L"静音", L"Mute");
        else if (on && (id==VoiceEq || id==Ducking || action==LoopRow)) title = L"✓  " + title;
        text(item.hDC,title,inset(r,3,0),fg,single|DT_CENTER,primary ? boldFont : nullptr);
        if (focus && primary) roundedFocus(item.hDC,inset(r,4,4),accentText);
    }
    void roundedFocus(HDC dc, RECT r, COLORREF c) const {
        const auto brush = SelectObject(dc,GetStockObject(NULL_BRUSH));
        const auto pen = SelectObject(dc,GetStockObject(DC_PEN));
        SetDCPenColor(dc,c); RoundRect(dc,r.left,r.top,r.right,r.bottom,px(6),px(6));
        SelectObject(dc,pen); SelectObject(dc,brush);
    }
    LRESULT drawSlider(NMCUSTOMDRAW& item) const {
        if (item.dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
        const HWND slider = item.hdr.hwndFrom;
        RECT r{}, thumb{}, channel{}; GetClientRect(slider,&r);
        SendMessageW(slider,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&thumb));
        SendMessageW(slider,TBM_GETCHANNELRECT,0,reinterpret_cast<LPARAM>(&channel));
        const int x = (thumb.left+thumb.right)/2, y = (r.top+r.bottom)/2;
        fill(item.hdc,r,panel);
        RECT rail{channel.left,y-px(2),channel.right,y+px(2)};
        rounded(item.hdc,rail,line,line,4);
        rail.right = x; if (rail.right>rail.left) rounded(item.hdc,rail,mint,mint,4);
        const int unity = (channel.left+channel.right)/2;
        fill(item.hdc,{unity,y-px(6),unity+1,y-px(3)},sub);
        rounded(item.hdc,{x-px(5),y-px(7),x+px(5),y+px(7)},mint,mint,10);
        if (GetFocus()==slider) roundedFocus(item.hdc,inset(r,1,1),mint);
        return CDRF_SKIPDEFAULT;
    }
    void drawCombo(HDC dc, HWND hwnd) const {
        RECT r{}; GetClientRect(hwnd,&r);
        const bool disabled = !IsWindowEnabled(hwnd);
        fill(dc,r,panel);
        rounded(dc,inset(r,1,1),deep,GetFocus()==hwnd ? mint : line);
        text(dc,caption(hwnd),inset({r.left,r.top,r.right-px(20),r.bottom},10,0),disabled ? dim : ink);
        const int x = r.right-px(15), y = (r.top+r.bottom)/2;
        const POINT mark[]{{x-px(4),y-px(2)},{x,y+px(2)},{x+px(4),y-px(2)}};
        stroke(dc,mark,3,disabled ? dim : mint);
    }
    void drawComboItem(const DRAWITEMSTRUCT& item) const {
        const bool selected = (item.itemState & ODS_SELECTED) != 0;
        fill(item.hDC,item.rcItem,selected ? raised : deep);
        if (item.itemID == static_cast<UINT>(-1)) return;
        const auto length = SendMessageW(item.hwndItem,CB_GETLBTEXTLEN,item.itemID,0);
        if (length<0) return;
        std::wstring str(static_cast<size_t>(length)+1,L'\0');
        const auto n = SendMessageW(item.hwndItem,CB_GETLBTEXT,item.itemID,reinterpret_cast<LPARAM>(str.data()));
        if (n<0) return;
        str.resize(static_cast<size_t>(n));
        text(item.hDC,str,inset(item.rcItem,9,0),selected ? mint : ink);
        if (item.itemState & ODS_FOCUS) roundedFocus(item.hDC,inset(item.rcItem,1,1),mint);
    }
    static LRESULT CALLBACK comboProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l,
                                      UINT_PTR id, DWORD_PTR data) {
        const auto* app = reinterpret_cast<const App*>(data);
        if (msg==WM_PAINT) {
            PAINTSTRUCT ps{}; const HDC dc = BeginPaint(hwnd,&ps);
            app->drawCombo(dc,hwnd); EndPaint(hwnd,&ps); return 0;
        }
        if (msg==WM_PRINTCLIENT) { app->drawCombo(reinterpret_cast<HDC>(w),hwnd); return 0; }
        if (msg==WM_ERASEBKGND) return 1;
        if (msg==WM_NCDESTROY) RemoveWindowSubclass(hwnd,comboProc,id);
        const auto result = DefSubclassProc(hwnd,msg,w,l);
        switch (msg) {
        case CB_SETCURSEL: case CB_RESETCONTENT: case WM_ENABLE: case WM_SETFOCUS: case WM_KILLFOCUS:
        case WM_KEYDOWN: case WM_LBUTTONUP: InvalidateRect(hwnd,nullptr,FALSE); break;
        }
        return result;
    }
