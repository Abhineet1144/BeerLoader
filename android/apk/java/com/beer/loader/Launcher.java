package com.beer.loader;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.text.InputType;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Start screen: the list of games (Windows executables), per-game extra environment, overlay and controller settings,
 * and Start. Start writes a session file in the launch.txt format (directory, exe, env:NAME=value lines) and opens the
 * native activity with its path; started directly (adb), the native activity still reads launch.txt instead.
 */
public class Launcher extends Activity {
    static final String SESSION_EXTRA = "beer.session";
    static final String[][] FIELDS = {
        {"fps", "FPS"}, {"frametime", "Frame time"}, {"cpu", "CPU usage"}, {"ram", "Memory (RAM)"},
        {"threads", "Thread count"}, {"speed", "Game speed"}, {"uptime", "Uptime"}, {"gpu", "GPU usage"},
        {"vram", "Video memory"}, {"clock", "Clock"}, {"graph", "Frame time graph"},
    };
    static final String[] CORNERS = {"Top left", "Top right", "Bottom left", "Bottom right"};
    static final String[] SIZES = {"Small", "Medium", "Large"};

    JSONObject config;            // launcher.json: {games:[{name,dir,exe,env}], selected, overlay:{...}, controller:{...}}
    LinearLayout gameList;
    EditText envEdit;
    int padding;

    File configFile() { return new File(getFilesDir(), "launcher.json"); }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        padding = (int) (12 * getResources().getDisplayMetrics().density);
        load();
        build();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refreshGames();
    }

    @Override
    protected void onPause() {
        super.onPause();
        save();
    }

    // ---- storage ---------------------------------------------------------------------------------------------

    void load() {
        try {
            config = new JSONObject(new String(Files.readAllBytes(configFile().toPath()), StandardCharsets.UTF_8));
        } catch (Exception e) {
            config = new JSONObject();
        }
        try {
            if (!config.has("games")) config.put("games", new JSONArray());
            if (!config.has("overlay")) {
                JSONObject o = new JSONObject();
                o.put("enabled", true);
                o.put("fields", new JSONArray(Arrays.asList("fps", "frametime", "cpu", "ram")));
                o.put("corner", 0);
                o.put("size", 1);
                config.put("overlay", o);
            }
            if (!config.has("controller")) {
                JSONObject c = new JSONObject();
                c.put("onscreen", true);
                config.put("controller", c);
            }
        } catch (Exception ignored) {
        }
    }

    void save() {
        try {
            if (envEdit != null) {
                JSONObject g = selectedGame();
                if (g != null) g.put("env", envEdit.getText().toString());
            }
            try (FileOutputStream out = new FileOutputStream(configFile())) {
                out.write(config.toString(2).getBytes(StandardCharsets.UTF_8));
            }
        } catch (Exception ignored) {
        }
    }

    JSONArray games() { return config.optJSONArray("games"); }

    JSONObject selectedGame() {
        int i = config.optInt("selected", -1);
        return i >= 0 && i < games().length() ? games().optJSONObject(i) : null;
    }

    // ---- layout ----------------------------------------------------------------------------------------------

    TextView heading(String text) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextSize(18);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setPadding(0, padding * 2, 0, padding / 2);
        return t;
    }

    void build() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(padding * 2, padding * 2, padding * 2, padding * 2);

        TextView title = new TextView(this);
        title.setText("Beer");
        title.setTextSize(28);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);

        if (!Environment.isExternalStorageManager()) {
            Button grant = new Button(this);
            grant.setText("Allow access to game files");
            grant.setOnClickListener(v -> startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName()))));
            root.addView(grant);
        }

        root.addView(heading("Games"));
        gameList = new LinearLayout(this);
        gameList.setOrientation(LinearLayout.VERTICAL);
        root.addView(gameList);
        Button add = new Button(this);
        add.setText("Add game (.exe)");
        add.setOnClickListener(v -> browse(Environment.getExternalStorageDirectory()));
        root.addView(add);

        root.addView(heading("Game options"));
        TextView envHelp = new TextView(this);
        envHelp.setText("Extra settings for the selected game, one NAME=value per line (for example BEER_BD12_FPS=1).");
        root.addView(envHelp);
        envEdit = new EditText(this);
        envEdit.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        envEdit.setMinLines(2);
        root.addView(envEdit);

        buildOverlay(root);
        buildController(root);

        Button start = new Button(this);
        start.setText("Start");
        start.setTextSize(20);
        start.setOnClickListener(v -> start());
        root.addView(start);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(root);
        setContentView(scroll);
        refreshGames();
    }

    void buildOverlay(LinearLayout root) {
        final JSONObject o = config.optJSONObject("overlay");
        root.addView(heading("Overlay"));
        Switch enabled = new Switch(this);
        enabled.setText("Show the performance overlay");
        enabled.setChecked(o.optBoolean("enabled", true));
        enabled.setOnCheckedChangeListener((b, on) -> put(o, "enabled", on));
        root.addView(enabled);

        List<String> chosen = new ArrayList<>();
        JSONArray f = o.optJSONArray("fields");
        for (int i = 0; f != null && i < f.length(); ++i) chosen.add(f.optString(i));
        for (String[] field : FIELDS) {
            CheckBox box = new CheckBox(this);
            box.setText(field[1]);
            box.setChecked(chosen.contains(field[0]));
            box.setOnCheckedChangeListener((b, on) -> {
                List<String> now = new ArrayList<>();
                JSONArray cur = o.optJSONArray("fields");
                for (int i = 0; cur != null && i < cur.length(); ++i) if (!cur.optString(i).equals(field[0])) now.add(cur.optString(i));
                if (on) now.add(field[0]);
                put(o, "fields", new JSONArray(now));
            });
            root.addView(box);
        }
        root.addView(labelled("Position", spinner(CORNERS, o.optInt("corner", 0), i -> put(o, "corner", i))));
        root.addView(labelled("Size", spinner(SIZES, o.optInt("size", 1), i -> put(o, "size", i))));
    }

    void buildController(LinearLayout root) {
        final JSONObject c = config.optJSONObject("controller");
        root.addView(heading("Controller"));
        Switch onscreen = new Switch(this);
        onscreen.setText("On-screen Xbox controller");
        onscreen.setChecked(c.optBoolean("onscreen", true));
        onscreen.setOnCheckedChangeListener((b, on) -> put(c, "onscreen", on));
        root.addView(onscreen);
    }

    interface IntChange { void set(int value); }

    Spinner spinner(String[] items, int selected, IntChange change) {
        Spinner s = new Spinner(this);
        s.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, items));
        s.setSelection(Math.max(0, Math.min(items.length - 1, selected)));
        s.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener() {
            public void onItemSelected(android.widget.AdapterView<?> p, View v, int pos, long id) { change.set(pos); }
            public void onNothingSelected(android.widget.AdapterView<?> p) {}
        });
        return s;
    }

    View labelled(String label, View view) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        TextView t = new TextView(this);
        t.setText(label);
        t.setMinWidth(padding * 10);
        row.addView(t);
        row.addView(view);
        return row;
    }

    static void put(JSONObject o, String key, Object value) {
        try { o.put(key, value); } catch (Exception ignored) {}
    }

    void refreshGames() {
        if (gameList == null) return;
        gameList.removeAllViews();
        JSONArray list = games();
        int selected = config.optInt("selected", -1);
        for (int i = 0; i < list.length(); ++i) {
            final int index = i;
            JSONObject g = list.optJSONObject(i);
            TextView row = new TextView(this);
            row.setText((i == selected ? "▶ " : "   ") + g.optString("name") + "\n      " + g.optString("dir"));
            row.setTextSize(16);
            row.setPadding(0, padding / 2, 0, padding / 2);
            row.setOnClickListener(v -> select(index));
            row.setOnLongClickListener(v -> { confirmRemove(index); return true; });
            gameList.addView(row);
        }
        if (list.length() == 0) {
            TextView none = new TextView(this);
            none.setText("No games yet. Add a game's .exe below; long-press a game to remove it.");
            gameList.addView(none);
        }
        JSONObject g = selectedGame();
        envEdit.setText(g != null ? g.optString("env") : "");
        envEdit.setEnabled(g != null);
    }

    void select(int index) {
        save();   // keeps the previous game's env text
        put(config, "selected", index);
        refreshGames();
    }

    void confirmRemove(int index) {
        JSONObject g = games().optJSONObject(index);
        new AlertDialog.Builder(this).setTitle("Remove " + g.optString("name") + "?")
                .setPositiveButton("Remove", (d, w) -> {
                    games().remove(index);
                    int selected = config.optInt("selected", -1);
                    put(config, "selected", selected == index ? -1 : selected > index ? selected - 1 : selected);
                    save();
                    refreshGames();
                }).setNegativeButton("Cancel", null).show();
    }

    // ---- file browser ----------------------------------------------------------------------------------------

    void browse(final File dir) {
        File[] entries = dir.listFiles();
        final List<File> shown = new ArrayList<>();
        if (dir.getParentFile() != null && !dir.equals(Environment.getExternalStorageDirectory())) shown.add(dir.getParentFile());
        if (entries != null) {
            Arrays.sort(entries, (a, b) -> a.isDirectory() != b.isDirectory() ? (a.isDirectory() ? -1 : 1)
                    : a.getName().compareToIgnoreCase(b.getName()));
            for (File e : entries)
                if (!e.getName().startsWith(".") && (e.isDirectory() || e.getName().toLowerCase().endsWith(".exe"))) shown.add(e);
        }
        String[] names = new String[shown.size()];
        for (int i = 0; i < names.length; ++i) {
            File e = shown.get(i);
            names[i] = (i == 0 && e.equals(dir.getParentFile())) ? ".. (up)" : e.isDirectory() ? e.getName() + "/" : e.getName();
        }
        new AlertDialog.Builder(this).setTitle(dir.getAbsolutePath())
                .setItems(names, (d, which) -> {
                    File e = shown.get(which);
                    if (e.isDirectory()) browse(e);
                    else addGame(e);
                }).setNegativeButton("Cancel", null).show();
    }

    void addGame(File exe) {
        try {
            JSONObject g = new JSONObject();
            String name = exe.getParentFile().getName();
            g.put("name", name.isEmpty() ? exe.getName() : name);
            g.put("dir", exe.getParent());
            g.put("exe", exe.getName());
            g.put("env", "");
            games().put(g);
            put(config, "selected", games().length() - 1);
            save();
            refreshGames();
        } catch (Exception ignored) {
        }
    }

    // ---- start -----------------------------------------------------------------------------------------------

    /** The flat JSON the loader's overlay reads from BEER_OVERLAY_JSON (platform/beer_overlay.c). */
    String overlayJson() throws Exception {
        JSONObject o = config.optJSONObject("overlay");
        JSONArray f = o.optJSONArray("fields");
        StringBuilder fields = new StringBuilder();
        for (int i = 0; f != null && i < f.length(); ++i) fields.append(i > 0 ? "," : "").append(f.optString(i));
        int corner = o.optInt("corner", 0);
        JSONObject flat = new JSONObject();
        flat.put("visible", o.optBoolean("enabled", true));
        flat.put("res_enabled", o.optBoolean("enabled", true));
        flat.put("fields", fields.toString());
        flat.put("res_x", (corner & 1) != 0 ? 0.8 : 0.01);
        flat.put("res_y", (corner & 2) != 0 ? 0.8 : 0.01);
        flat.put("res_size", o.optInt("size", 1));
        flat.put("pin_enabled", false);
        return flat.toString();
    }

    void start() {
        save();
        JSONObject g = selectedGame();
        if (g == null) {
            new AlertDialog.Builder(this).setMessage("Add and select a game first.").setPositiveButton("OK", null).show();
            return;
        }
        try {
            StringBuilder s = new StringBuilder();
            s.append(g.optString("dir")).append('\n').append(g.optString("exe")).append('\n');
            s.append("env:BEER_OVERLAY_JSON=").append(overlayJson()).append('\n');
            s.append("env:BEER_ANDROID_ONSCREEN_PAD=").append(config.optJSONObject("controller").optBoolean("onscreen", true) ? "1" : "0").append('\n');
            for (String line : g.optString("env").split("\n")) {
                line = line.trim();
                if (line.contains("=")) s.append("env:").append(line).append('\n');
            }
            File session = new File(getFilesDir(), "session.txt");
            try (FileOutputStream out = new FileOutputStream(session)) {
                out.write(s.toString().getBytes(StandardCharsets.UTF_8));
            }
            Intent intent = new Intent(this, GameActivity.class);
            intent.putExtra(SESSION_EXTRA, session.getAbsolutePath());
            intent.putExtra(GameActivity.PAD_EXTRA, config.optJSONObject("controller").optBoolean("onscreen", true));
            startActivity(intent);
        } catch (Exception e) {
            new AlertDialog.Builder(this).setMessage("Cannot start: " + e).setPositiveButton("OK", null).show();
        }
    }
}
