package main

import "time"

func (m *model) handleManagement(e keyEvent, now time.Time) ([]soundCommand, string) {
	if !e.Down {
		return nil, ""
	}
	switch e.Code {
	case 105:
		if m.ManagerIndex > 0 {
			m.ManagerIndex--
		}
	case 108:
		if m.ManagerIndex+1 < len(m.ManagerItems) {
			m.ManagerIndex++
		}
	case 28, 352:
		if len(m.ManagerItems) > 0 {
			x := m.ManagerItems[m.ManagerIndex]
			if x.IsWAV {
				return nil, "manager-play:" + x.Path
			}
			return nil, "manager-open:" + x.Path
		}
	case 14, 111:
		if len(m.ManagerItems) > 0 {
			x := m.ManagerItems[m.ManagerIndex]
			if x.IsWAV {
				return nil, "manager-delete:" + x.Path
			}
		}
	case 19: // R creates a fresh archive without requiring text input.
		return nil, "manager-create"
	case 16: // Q is save-as while management screen is open.
		return nil, "manager-saveas"
	case 38, 53, 50:
		m.Management = false
	}
	return nil, ""
}
