function value = onPulse(source, eventData)
    value = source.record(eventData.EventName);
end
