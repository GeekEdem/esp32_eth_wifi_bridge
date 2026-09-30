# Runtime prelude, compiled before the user script. Keep it small: it lives
# in the script's memory budget.
import json

var _timers = {}
var _timer_seq = 0

def every(ms, fn)
  _timer_seq += 1
  _timers[_timer_seq] = [millis() + ms, ms, fn]
  return _timer_seq
end

def after(ms, fn)
  _timer_seq += 1
  _timers[_timer_seq] = [millis() + ms, 0, fn]
  return _timer_seq
end

def cancel(id)
  if _timers.contains(id) _timers.remove(id) end
end

def status()
  return json.load(_status_json())
end

# Called by the runtime: runs due timers, returns ms until the next one (-1: none).
def _run_timers()
  var now = millis()
  var due = []
  for id: _timers.keys()
    if _timers[id][0] <= now due.push(id) end
  end
  for id: due
    if _timers.contains(id)
      var t = _timers[id]
      if t[1] > 0
        t[0] = now + t[1]
      else
        _timers.remove(id)
      end
      t[2]()
    end
  end
  var wait = -1
  now = millis()
  for t: _timers
    var w = t[0] - now
    if w < 0 w = 0 end
    if wait < 0 || w < wait wait = w end
  end
  return wait
end
