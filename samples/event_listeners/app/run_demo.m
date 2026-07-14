pulse = signals.Pulse();

factor = 4;
transform = @(value) value * factor;
factor = 10;
closure_value = transform(3);

anonymous_listener = addlistener(pulse, 'Tick', ...
    @(source, eventData) source.record(eventData.EventName));
package_callback = @signals.onPulse;
package_listener = addlistener(pulse, 'Tick', package_callback);

first_count = pulse.emit(2);
callbacks_after_first = pulse.CallbackCount;

anonymous_listener.Enabled = false;
second_count = pulse.emit(3);
callbacks_while_disabled = pulse.CallbackCount;

delete(package_listener);
anonymous_listener.Enabled = true;
third_count = pulse.emit(4);
callbacks_after_delete = pulse.CallbackCount;

private_callback = pulse.privateCallback();
private_listener = addlistener(pulse, 'Tick', private_callback);
fourth_count = pulse.emit(1);
callbacks_after_private = pulse.CallbackCount;

event_names = events(pulse);
first_event = event_names{1};
visible_event_count = numel(event_names);
listener_valid = isvalid(anonymous_listener);
last_event = pulse.LastEvent;

summary = closure_value + fourth_count + callbacks_after_private + ...
    visible_event_count + listener_valid;
